/* Flow-IPC: SHM-jemalloc
 * Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
 * Each commit is copyright by its respective author or author's employer.
 *
 * Licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */

/// @file
#pragma once

#include "ipc/shm/arena_lend/arena_lend_stats.hpp"
#include "ipc/shm/arena_lend/detail/arena_lend_fwd.hpp"
#include "ipc/common.hpp"
#include <flow/util/thread_lcl.hpp>
#include <flow/util/util.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <flow/util/stat/stat_set_list.hpp>
#include <flow/util/util_fwd.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/thread/tss.hpp>
#include <boost/move/unique_ptr.hpp>
#include <boost/range/join.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <mutex>
#include <memory>

namespace ipc::shm::arena_lend::detail::stat
{

// Types.

/**
 * (Internal-use) Singleton (per-`Shm_arena` type <=> per-arena-lending SHM-provider -- like SHM-jemalloc)
 * into which owners of per-arena TL-shards (of type Sharded_stats) can deposit such TL-shards that
 * are finalized (will never be updated with new measurements again) + can no longer be stored in TL-state objects;
 * and from which stat-consumption code can read and still aggregate these finalized but still fully valid
 * stat-shards.
 *
 * That was barely English; so in this case it might be best just to see who uses it and why.  Namely as of this
 * writing:
 *   - Thread_lcl_obj_db_admin::~Thread_lcl_obj_db_admin() dtor -- executed when an `_admin`-pertaining
 *     user thread happens to exit but has some per-arena stat-shards that would otherwise no longer
 *     participate in aggregation.  The thread exiting does not mean the arena is gone.
 *     - In `~_admin()` dtor, we may be forced to (in a sense) "prolong" the user thread by spawning
 *       our own Thread_lcl_obj_db_admin::degraded_admin_thread_body() thread that will continue
 *       eliminating stuff, until no live-objects remain, and *that* thread can exit.  Then `~_admin()` runs *again*.
 *       In that case, it is only that 2nd dtor invocation that exercises this singleton.
 *       If not forced to do that, then the 1st and only dtor invocation does so.
 *   - Thread_lcl_obj_db_client::~Thread_lcl_obj_db_client() dtor -- executed when a `_client`-pertaining
 *     user thread happens to exit but has some per-arena stat-shards that would otherwise no longer
 *     participate in aggregation.  The thread exiting does not mean the arena is gone.
 *
 * @note Incidentally, how is it safe for `_admin` or `_client` to even remove a stat-set from itself in its dtor so as
 *       to dump it here?  By definition, it must not be due to arena shutting down (which would be fine), since
 *       dumping it here => we are interested in it.  The answer is that in such a dtor that `_admin` or `_client`
 *       is no longer in the `Thread_local_state_registry`, so aggregation cannot target it at that moment.
 *       Yes, this implies there's a short time when the stat-set is neither in the TL-states set nor in
 *       `*this` singleton, and stat-consumption would "miss" it and be buggy.  This is discussed elsewhere, but in
 *       short we avoid it by using a special mutex dedicated to this gap.
 *
 * So this simple facility has these properties:
 *   - Singleton, segregated by `Shm_arena` type.
 *     - On-demand lazy creation (avoids `static`-init hell): get().
 *   - stats_insert() to fold a Sharded_stats shard, that is about to be otherwise forgotten, into the
 *     arena's single *accumulated finalized shard*.  (Folding -- field-wise summing -- loses nothing:
 *     every reader here is sum-shaped; see #m_per_arena_stats_shards doc header.  This is *almost* a white-box
 *     detail -- one way or another the shard is saved into `*this` -- except it can be observed, in that
 *     stats_while_locked() will supply just 1 Sharded_stats, 1+ source-shards having been folded into it.)
 *   - At stat-consumption for a given arena: stats_while_locked() provides access to that shard.
 *   - At stat-reset for a given arena: the reset operation (see sharded_stats_reset()) walks the saved shards --
 *     via stats_while_locked() -- zeroing the ACCUMULATORs within each, as it does for all shards everywhere.
 *     (If only ACCUMULATORs existed, the shards could instead just all be deleted.)
 *     - In this sense and -- as of this writing, that we know of -- this sense only, the shards are not
 *       quite "finalized" necessarily.  Conceptually this is not really an exception; the word "finalized"
 *       means that the quantities measured are finalized; a stat-reset does not change those measurements
 *       but rather the time period over which they apply -- from (whatever it was before) to (starting from
 *       the time of the reset).
 *   - If forgetting an arena fully (meaning it is shut down -- no stat-consumption/resets will ever happen again):
 *     stats_erase() will erase the node from `*this`.  (This is just for cleanliness/unleakiness.)
 *   - All operations are thread-safe (indeed they will be coming from every manner of thread).  We achieve this
 *     via internal mutex.
 *     - Perf: All ops above are rare; so this is all fine.
 *
 * @tparam Shm_arena_t
 *         See Thread_lcl_obj_db_admin and/or Thread_lcl_obj_db_client.  In our case this type is fully used
 *         as a compile-time discriminator so as to segregate the singletons; we do not use its API.
 */
template<typename Shm_arena_t>
class Finalized_shards
{
public:
  // Types.

  /// Short-hand for template parameter type.
  using Shm_arena = Shm_arena_t;

  /// Short-hand for single-ownership pointer (`unique_ptr` of some sort).
  template<typename T>
  using Own = boost::movelib::unique_ptr<T>;

  // Methods.

  /**
   * Returns the process-wide singleton instance of this class.  It is created in thread-safe fashion the
   * first time this is called.  It is an immortal (intentionally never-destroyed) singleton -- see the impl
   * comment in the body -- so it is safe to call at any time after the first call, from any thread, including
   * during thread or `static` teardown.
   *
   * @return Reference to the singleton.  Always the same address; the addressee is valid at all times.
   */
  static Finalized_shards& get_instance();

  /**
   * Provides access -- thread-safely -- to the arena's accumulated finalized shard: the fold (field-wise
   * sum) of all shards `stats_insert()`ed (and not `stats_erase()`ed) pertaining to the given arena.
   * The mechanism for this is as follows:
   *   -# Caller prepares, as `task`, the code they plan to execute while we hold the lock that
   *      prevents simultaneous modification via stats_insert(), stats_erase(), or
   *      stats_while_locked() itself.
   *   -# Caller calls `stats_while_locked(task)`.
   *      -# This method locks the aforementioned lock.
   *      -# It calls `task(fold_shard_or_null)`, with `Sharded_stats* fold_shard_or_null` pointing to the
   *         *mutable* accumulated shard for the desired arena (`collection_id`).
   *         - If no shard has been saved for `collection_id`, it is null.
   *      -# It unlocks lock.
   *
   * @warning Attention!  The null case mentioned above is not an error or even unusual;
   *          it is after all entirely possible that for that arena no shards have been finalized at this
   *          time -- e.g., no relevant `Thread_lcl_obj_db_*` threads have exited.
   *
   * @note The pointee shard is mutable.  You *can* modify it if desired, though obviously this
   *       should be done with care if at all.  We could have provided a `const` overload that supplies
   *       access to `const Sharded_stats`, but it felt less fussy and more straightforward
   *       to keep it simple like that -- particularly for an internal-use module.
   *
   * @see sharded_stats_reset(), in point of fact, reaches the shards through stats_while_locked() so as to
   *      modify them in that particular fashion (zeroing ACCUMULATORs).  As of this writing we are not aware
   *      of any other legitimate reasons to modify shards -- which by conception are "finalized."
   *
   * Behavior is undefined (actually: deadlock) if task() calls `this->stats_while_locked()` (the mutex is
   * non-recursive).
   *
   * @tparam Task
   *         Function object matching signature implied above.
   * @param collection_id
   *        See stats_insert().
   * @param task
   *        This will be invoked as follows: `task(fold_shard_or_null)`.  See above.
   */
  template<typename Task>
  void stats_while_locked(collection_id_t collection_id, const Task& task);

  /**
   * Folds (field-wise-sums, a-la `flow::util::stat::stats_aggregate_one()`) the given stats shard into the
   * given arena's accumulated finalized shard -- creating the latter if none exists yet.  `Thread_lcl_obj_db_*`
   * stat aggregation at consumption time shall include this contribution until erased via stats_erase().
   * (Folding, versus storing shards individually, loses nothing; see #m_per_arena_stats_shards doc header.)
   *
   * The shard's values are taken via `flow::util::stat::stats_assign()`/`stats_aggregate_one()`, per standard
   * `flow::util::stat` policy.
   *
   * ### Rationale: Why copy? ###
   * That is, why not use #Own wrapping instead, so it can be `move()`d?  Answer: It's essentially
   * to maximize perf in the caller module(s), so they are not forced to use the heap more and maintain
   * an extra pointer indirection along various fast-paths.  Naturally the `stats_assign()` required here
   * will itself be slow -- much slower in its than any single pointer indirection -- but stats_insert()
   * is a rare event not along any conceivable fast-path (threads don't exit that often).
   *
   * @param collection_id
   *        `Shm_arena::get_id()` that identifies the arena to which `stats_shard` pertains.
   *        It is allowed and normal for shard(s) with the same ID (from the same arena) to be in `*this`
   *        already.  E.g., 1+ different threads shall have shards about the same arena and could all
   *        at some point exit and eventually need to be saved in `*this`.
   * @param stats_shard
   *        The shard.
   */
  void stats_insert(collection_id_t collection_id, const Sharded_stats& stats_shard);

  /**
   * Deletes the accumulated finalized shard (all `stats_insert()`ed contributions) pertaining to the given
   * arena.  Use case: when that arena
   * no longer exists <=> its aggregated stats are not to be stat-consumed subsequently.
   *
   * @param collection_id
   *        See stats_insert().
   */
  void stats_erase(collection_id_t collection_id);

private:
  // Constructors/destructor.

  /// Constructor.  Zero-arg due to singleton pattern.
  Finalized_shards();

  // Data.

  /// Protects #m_per_arena_stats_shards;
  mutable flow::util::Mutex_non_recursive m_mutex;

  /**
   * Stores, per arena, the arena's *accumulated finalized shard*: the fold -- field-wise sum, in the
   * `flow::util::stat::stats_aggregate_one()` sense -- of every shard `stats_insert()`ed (and not
   * `stats_erase()`ed) for that arena.  A single fold per arena suffices -- folding loses nothing -- because
   * every reader of finalized shards is sum-shaped: the stat-consumption walk sums ACCUMULATORs and GAUGEs
   * across all shards, never reading per-shard HI_WMARKs (see `flow::util::stat::stats_aggregate_shards()`);
   * and the stat-reset walk zeroes ACCUMULATORs, which distributes over the fold.  Meanwhile the fold keeps
   * `*this` O(1) in space and walk-time per arena, regardless of how many threads have come and gone -- which,
   * for a thread-churning application with a long-lived arena, coceivably matters.
   *
   * We don't sweat perf too much, given that all our methods are called relatively rarely/never in fast-paths.
   * We make a decent attempt at it though.
   *   - The `Own<>` wrapper: the shard type is not copyable (atomics), while `unordered_flat_map` moves/copies
   *     values around on rehash; the heap-pinning squares that circle.
   *   - Constant-time lookup and other nice perf characteristics => use the best available hash-map which is
   *     `unordered_flat_map`.  Pointer stability in and of itself is unnecessary.
   *
   * Invariant: Each stored `Own<>` is non-null (created, pointing to the fold, by stats_insert(); removal is
   * only via stats_erase(), which erases the entire map node).
   *
   * @note It is perhaps tempting to store `Own<const Sharded_stats>`, as the shards are
   *       *finalized*; but actually the meaning of the word finalized is slightly different than
   *       "actually immutable"; at least the stat-reset operation (which zeroes ACCUMULATORs within each
   *       shard) shows this fact.  (See class doc header for brief discussion.)
   */
  boost::unordered_flat_map<collection_id_t, Own<Sharded_stats>> m_per_arena_stats_shards;
}; // class Finalized_shards

// Free functions: in *_fwd.hpp.

// Template implementations: free functions.

template<typename Shm_arena>
void sharded_stats(const Shm_arena& shm_arena, Sharded_stats* target_stats)
{
  sharded_stats_impl<true>(shm_arena, target_stats);
}

template<typename Shm_arena>
void sharded_stats_reset(const Shm_arena& shm_arena, Sharded_stats* target_stats)
{
  sharded_stats_impl<false>(shm_arena, target_stats);
}

template<bool CONSUME_ELSE_RESET, typename Shm_arena>
void sharded_stats_impl(const Shm_arena& shm_arena, Sharded_stats* target_stats)
{
  using flow::util::stat::stats_aggregate_shards;
  using flow::util::stat::stats_reset_shard_aggregate;
  using flow::util::Lock_guard;
  using flow::util::Mutex_recursive;
  namespace range = boost::range; // @todo There's some ADL-feeling ambiguity versus algorithm/string/join.hpp....
  using boost::adaptors::map_keys;
  using boost::adaptors::filtered;
  using boost::adaptors::transformed;

  /* Firstly: We are _impl(), meaning we are the body of sharded_stats() (stats-consumption) and sharded_stats_reset()
   * (stats-reset).  At this point take a look at doc headers for flow::util::stats_aggregate_shards() and
   * stats_reset_shard_aggregate() respectively.  They are quite similar; the args are:
   *   - Stat_set* target_stats: Just as it says.
   *   - const It& src_stats_begin/end: The range -- possibly empty! -- of extant TL-shard `Stat_set`s.
   *     How to get at this range is a whole thing (we'll get into that just below), but the point at this time is:
   *     It is the same range in either case.
   *   - One of (for stat-consume and stat-reset respectively):
   *     - const Stat_set* fresh_stats_or_null: Source of init-values (zeroes, more or less); but required if and only
   *       if the in-range is empty.  (If it's non-empty, then it'll copy shard 1, += shard 2, += shard 3, ....)
   *     - const Stat_set& fresh_shard: Same thing but always required (which makes sense... we're resetting stuff).
   *
   * So really regardless of CONSUME_ELSE_RESET, we're about to do the same thing -- until the very last moment
   * wherein we actually call stats_*aggregate*().  At that point we'll call one or the other and give it the
   * proper final arg.
   *
   * Suggestion: If trying to grok for the first time, just assume CONSUME_ELSE_RESET=true and follow along.
   *
   * ---
   *
   * The following is pretty straightforward-looking, at least relatively speaking.  It has to run through 3
   * quite-differently-structured containers in one iterator-range, so there is some range-fu required, but
   * algorithmically it gets right to the point without any real digressions or corner cases.
   *
   * Point is: The hard parts have all been done; very much so that this actual step of pulling together all
   * the different shards (comprising Thread_lcl_obj_db_admin/client/finalized_shards) can be indeed straightforward.
   *
   * Advice as to how to grok this whole setup: A good place to start is the Sharded_stats
   * doc header; then here; and then finally see how the sausage is made inside the `// Stats.` {blocks} of
   * Thread_lcl_obj_db_admin/client/finalized_shards.  flow::util::stat doc header, especially the
   * section about concurrency/sharding is arguably required background too.
   *
   * Note that we're using `friend` access to _admin and _client, though as noted near the `friend` decls
   * it is meant not generally but specifically to be able to do
   * Thread_local_state_registry<_admin/_client>::while_locked(S), where S is basically the
   * set of _admin/_client TL-state objects extant; and in each _admin and _client in S, access
   * the stats-shard _stats for `shm_arena`.  Then we stats_aggregate_shards() or stats_reset_shard_aggregate() 'em
   * (these _stats shards).
   *
   * (Finalized_shards accumulates "finalized," ejected -- but still fully relevant --
   * shards from both _admin and _client; and, being a custom fully-stats-focused class -- unlike
   * _admin/_client which do stats but mainly much other stuff -- provides a civilized public API.
   * No friendship needed for it.  But for _admin and _client we went with `friend`ship; rare in
   * Flow-IPC admittedly, but here it seems to make sense, particularly among internal-use modules.)
   *
   * OK... let's go. */

  using Shard = Sharded_stats;
  using Admin = Thread_lcl_obj_db_admin<Shm_arena>;
  using Client = Thread_lcl_obj_db_client<Shm_arena>;
  using Fin_shards = Finalized_shards<Shm_arena>;
  const auto collection_id = shm_arena.get_id();

  /* Tactical range-fu explanation:
   *
   * The goal is to get every Shard, for shm_arena.get_id()), from Admin, Client, and Fin_shards (the latter
   * being the simple one: Fin_shards::stats_while_locked(collection_id) will give us its lone fold-shard
   * pointer -- or null).  Firstly we need to lock each of {Admin|Client|Fin_shards} global mutex while grabbing the
   * shards list for that guy; do that using Thread_local_state_registry<Admin/Client>::while_locked(F) and
   * Fin_shards::stats_while_locked(collection_id, F).  Inside F() the respective mutex is locked.
   *
   * Secondly we need to give that Shard-list to stats_aggregate_shards()/stats_reset_shard_aggregate() in a particular
   * form: the iterators [B, E) of a type that'll go through the conceptual Shard-list... with *all 3* sub-lists.
   *
   * Moreover: in the case of stats_reset_shard_aggregate(), the iterator pointee is *mutable* (Shard&,
   * not const Shard&): it will reset each ACCUMULATOR in each Shard.  This is important in that we cannot
   * merely make a copy of each Shard in some new temp container and run through that.  We have to actually
   * run through them in-place, once.  (The stats_update_pre_consumption() thing in there makes this even more
   * desirable.)  So: Given there's essentially one way to do it:
   *
   * Use boost::range et al.  (Need C++2x for std:: equivalent, though the required views::concat
   * is C++26.  Don't quote me -- ygoldfel -- on those details, but definitely C++20 at least is needed.  We
   * are on C++17 as of this writing.)  This is pretty slick, as it won't need to make any new container in
   * memory; it can be made to walk through all 3 rather-mutually-different containers and fish out the right
   * things and ignore the wrong things (where shm_arena has no data).  So let's go with that.
   *
   * We'll just need some of the little pieces as lambdas.  It should be self-explanatory, mostly, if one knows
   * how ranges/adaptors work. */

  /* A key thing is to take an Admin or Client and its Shard for `collection_id` arena.
   * However, a given Admin/Client might not have a Shard for that arena.  So for now we'll generate
   * a Shard* instead; a nullptr if no Shard for our arena in that Admin/Client.  Can easily eliminate the
   * nulls via `filtered` adapter "after" that.  (A nice thing is that it's not really "after" -- it only
   * looks like that in the code -- in reality once the null is detected it is immediately skipped; and indeed
   * stats_aggregate_shards()/stats_reset_shard_aggregate() walks the range exactly once, hence the null-check
   * occurs but once per TL-state.)
   *
   * Fin_shards will already give us a Shard*-or-null -- the same shape as for Admin/Client and handled the
   * same way (filter out the null; deref).
   *
   * These 2 things do it for Admin and Client respectively. */
  const auto admin_to_shard_ptr = [collection_id](Admin* admin) -> Shard*
  {
    /* Ah, we must not forget that a small subset of things in each Admin's for-`shm_arena` Shard (if any)
     * are not live-updated and must be set at stat-consumption time; which is now.  The method
     * Admin::stats_update_pre_consumption() takes care of it.  (It also checks whether *admin
     * has a shm_arena Shard; and fishes out its address if so -- both core tasks of this lambda.)
     * (It has a documented requirement: shm_arena must not be shutting down.  No prob: That's our documented
     * requirement as well.)
     *
     * Caution!  This only works smoothly, because stats_aggregate_shards() contract promises to walk the range
     * only once.  Therefore we check for lack of map[collection_id] exactly once per Admin: right below inside
     * stats_update_pre_consumption(); and then either entirely ignore it, or entirely don't ignore it:
     * massage the Shard *and* return non-null here, thus causing it to also count in the aggregation.
     *
     * If the walking happened 2+ times: The massaging would either pointlessly occur 2+ times, or we'd need to
     * guard against it in ugly fashion.
     *
     * So again... caution!  Keep this all in mind when grokking and/or maintaining. */

    return admin->stats_update_pre_consumption(collection_id); // Modifies pointee of returned ptr; or returns null.
  }; // const auto admin_to_shard_ptr =

  // Similar deal but no massaging and just generally less indirection inside *client versus *admin.
  const auto client_to_shard_ptr = [collection_id](Client* client) -> Shard*
  {
    const auto& map = client->m_per_arena_stats_shards;
    { /* Gotta lock this thing: `map` key-set may be being modified by new_pool_data()
       * or if_requested_forget_arena_related_resources(). */
      Lock_guard<decltype(client->m_per_arena_stats_shards_mutex)> lock{client->m_per_arena_stats_shards_mutex};
      const auto it_collection_id_and_stats = map.find(collection_id);
      return (it_collection_id_and_stats == map.end()) ? static_cast<Shard*>(nullptr)
                                                       : it_collection_id_and_stats->second.get();
    }
  };

  /* The algorithm stats_aggregate_shards()/stats_reset_shard_aggregate() needs `Shard&`s range.
   * For Admin/Client we have Shard*; for Fin_shards we have Own<Shard>; either way the conversion is just
   * the *p deref op. */
  const auto shard_ptr_to_shard_ref = [](const auto& shard_ptr) -> Shard& { return *shard_ptr; };

  // Lastly this will help throw out those arena-not-tracked nulls from Admin/Client.
  const auto shard_ptr_non_null = [](Shard* shard_ptr) -> auto { return bool(shard_ptr); };

  /* Outermost lock: Full explanation for this is in thread_end_gap_mutex() doc header.  In short:
   *
   * We serialize this walk against Admin and Client TL-state teardown (at thread end).  The teardown
   * begins before the first Admin or Client is removed from its Thread_local_state_registry; and ends after
   * the last `Shard` (from among all `Admin`s and `Client`s shutting down with the thread) has been placed
   * moved into either Finalized_shards or a replacement-`Admin` (the latter already having been inserted
   * into _registry).
   *
   * Without that measure we could here miss 0+ `Shard`s from 0+ `Admin`s and 0+ `Client`s: A given Shard
   * might be, for a split second, in no Admin or Client or Finalized_shards, even though it'll absolutely
   * end up there.  So this ensures we do the walk strictly before or strictly after this short gap. */
  Lock_guard<Mutex_recursive> gap_lock{thread_end_gap_mutex()};

  // Since we're walking through all 3 in one go, no choice but to lock all 3 (not that it's an actual perf issue).
  Admin::s_state.m_obj_db_registry.while_locked([&](const auto& admin_state_and_mdt_map)
  {
    Client::s_state.m_obj_db_registry.while_locked([&](const auto& client_state_and_mdt_map)
    {
      Fin_shards::get_instance().stats_while_locked(collection_id,
                                                    [&](Shard* fin_fold_shard_or_null)
      {
        const auto admin_client_finalized_shards
          = range::join
              (range::join
                 (admin_state_and_mdt_map | map_keys
                    | transformed(admin_to_shard_ptr) | filtered(shard_ptr_non_null)
                    | transformed(shard_ptr_to_shard_ref),
                  client_state_and_mdt_map | map_keys
                    | transformed(client_to_shard_ptr) | filtered(shard_ptr_non_null)
                    | transformed(shard_ptr_to_shard_ref)),
               boost::make_iterator_range(&fin_fold_shard_or_null, &fin_fold_shard_or_null + 1)
                 | filtered(shard_ptr_non_null) | transformed(shard_ptr_to_shard_ref));
        /* The resulting range-iterator will now (inside stats_aggregate_shards()):
         *   - walk through all extant `Admin`s, skipping those where [collection_id] is not a key;
         *     for each one fish out the Shard*, massage it (stats_update_pre_consumption()), and feed it
         *     into aggregation as a Shard&; then
         *   - walk through all extant `Client`s, skipping those where [collection_id] is not a key;
         *     for each one fish out the Shard*, and feed it into aggregation as a Shard&; then
         *   - walk through the finalized fold-shard, if any, feeding it into aggregation as a Shard&.
         *
         * For stats_reset_shard_aggregate(): As written.
         * For stats_aggregate_shards(): As written; just that the iterator pointee is Shard&, but
         * that function will treat it as `const Shard&` (won't be modifying the `Shard`s). */
        const auto& begin = admin_client_finalized_shards.begin();
        const auto& end = admin_client_finalized_shards.end();
        if constexpr(CONSUME_ELSE_RESET)
        {
          if (begin == end)
          {
            Shard fresh_stats_from_0_shards;
            /* See explanation of fresh_stats_from_0_shards arg in stats_aggregate_shards() doc header.
             * We are exercising that "advanced trick" here.  In short: when there are no shards, the
             * proper value for m_n_shards is self-evidently zero; but in Shard{} -- used when constructing
             * a new TL-shard -- it is 1.  So we tweak it: */
            fresh_stats_from_0_shards.m_owner_obj.m_n_shards = 0;
            stats_aggregate_shards<Shard>(target_stats, begin, begin, &fresh_stats_from_0_shards);
          }
          else
          {
            stats_aggregate_shards<Shard>(target_stats, begin, end, nullptr);
          }
        }
        else // if constexpr(!CONSUME_ELSE_RESET)
        {
          Shard fresh_stats_from_0_shards;
          fresh_stats_from_0_shards.m_owner_obj.m_n_shards = 0; // Same deal as just above.
          stats_reset_shard_aggregate(target_stats, begin, end, fresh_stats_from_0_shards);
        } // else // if constexpr(!CONSUME_ELSE_RESET)
      }); // Fin_shards::get_instance().stats_while_locked()
    }); // Client::s_state.m_obj_db_registry.while_locked()
  }); // Admin::s_state.m_obj_db_registry.while_locked()
} // sharded_stats_impl()

// Template implementations: Finalized_shards.

template<typename Shm_arena_t>
Finalized_shards<Shm_arena_t>&
  Finalized_shards<Shm_arena_t>::get_instance() // Static.
{
  static auto s_fin_shards = new Finalized_shards;
  return *s_fin_shards;

  /* The local-static technique is thread-safe in C++17 at least.  It's helpful from the init direction (deinit
   * is discussed below), as there's no need to guarantee any particular static init ordering w/r/t objects
   * that need us; it's created one time on-demand/lazily, then snappily used from that point on (fast-path).
   *
   * As for deinit:
   *
   * You'll notice it's not written simply as: `static Finalized_shards s_fin_shards;`
   * Instead it allocates the object in heap, whereas the `static` is just the pointer.  Effect: When static deinit
   * of s_fin_shards happens around program exit, the deinit = no-op; the static thing is just a pointer.  Hence
   * the _finalized_shards stays around for a (likely short) time.  Finally, once everything C++-y has been
   * cleaned up according to its complex ordering rules around this, the OS reclaims anything left-over that
   * has not been cleaned up via deinit; in this case that amounts to the heap-memory backing the data structures
   * in this single (per Shm_arena type in play, usually one but not necessarily) _finalized_shards object.
   * Specifically that's: a mutex object and a map of (heap-pinned) stats-structs.  So that memory is given back
   * to the OS.
   *
   * Why do it this way?  Aesthetically, at least, it seems somewhat gross -- a leak of sorts.  Answer is
   * two-fold: Why it's fine; and why the alternative is worse.
   *
   * ### Why it's fine ###
   * A bunch of _admin and _client objects need it potentially throughout the application's
   * lifetime, and it is difficult to predict exactly when that would be.  As of this writing _admin and _client
   * need it, potentially, in their dtors, which run at user thread end, or in _admin's case *potentially*
   * at degraded-replacement-admin-thread (degraded_admin_thread_body()) end, which occurs when the user's use
   * patterns allow it to (by letting go of live objects originaly constructed in the replacee-thread(s)).
   * Anyway hopefully it's evident that it's hard to predict when exactly that might be, particularly around
   * program exit; there are various corner-case possibilities about the user's thread structure and other matters.
   *
   * That is all to say: lots of different objects will need it, potentially at weird times, very much possibly
   * around program exit... and that's when static + thread-local deinit order rears its unpleasant-looking head.
   * _client and _admin objects are all thread-local, and their dtors use this _finalized_shards.  However,
   * we've effectively made s_fin_shards have no deinit; by definition OS will reclaim this memory/the contained
   * objects no earlier than any C++-y things (dtors in our case) have run.  So it'll work.
   *
   * Isn't it a leak though?  Yes but not really.  It *is* only RAM (there aren't any sockets/FDs/SHM-pool handles
   * in there; again it is a mutex and a map of stats-structs).  Also to the point: the actual
   * stats-structs in the map -- presumably accounting for most of the actual RAM used -- *are* cleaned-up at
   * the earliest opportunity in civilized fashion: whenenver an arena is shut down, _admin::forgetting_shm_arena()
   * will immediately delete its node from the map (so the fold-struct goes away).  That leaves
   * just the map husk + mutex, hardly a big deal.  All we're doing is keeping it (also the mutex) around longer
   * (just a little longer at that) than its potential users might need it.
   *
   * ### Why the alternative is worse ###
   * The main alternative is to declare it as `static ..._finalized_shards s_fin_shards;` instead.
   * Is it better?  Well, potentially, it's better in that aesthetics are such that things that are cted should be dted,
   * and not doing that is naughty.  Effectively, though, the result is no different: it's just freed memory,
   * whether via dtor near-program-end or via OS-reaping near-program-end.
   *
   * Is it worse?  Answer: In terms of functionality... yes, considerably.  We mentioned above what uses it.
   * If the deinit of s_fin_shards occurs before the deinit, via notoriously potentially-chaotic deinit patterns
   * (around program exit), of even one of those TL-states (an _admin or a _client) => crash, usually in the form
   * of a pthread/mutex error, trying to access an invalid mutex _finalized_shards::m_mutex.  Unfortunately, both
   * theoretically and more importantly empirically, that does happen.  The problem is it's difficult to wrangle
   * as of this writing.  _admin and _client are thread-local, which is tricky enough potentially, but as I write
   * this they're not `thread_local` but rather controlled (by way of Thread_local_state_registry) by
   * boost::thread_specific_ptr.  Last I (ygoldfel)  checked in Linux it's doing it via pthread TL-oriented calls
   * (plus a global std::map).  (There are some plans to use `thread_local` instead, mainly for other reasons, but
   * at the moment just plans.)
   *
   * So the bottom line is, things *do* go bad if done that way, and whatever steps one would take to fight that
   * would involve non-trivial investigation and dev.  Whereas, as written, it just works -- without "leaking"
   * hardly anything that wouldn't be "leaked" regardless, given when _admin and _client deinit can occur in
   * practice.
   *
   * ### One more thing ###
   * In ~_admin() and ~_client() dtor, in similar ways, there's a check that should bypass all or at least most
   * of all this entropy.  It is simply that, if there are no per-arena data left in the _admin/_client, then
   * there's nothing to dump into s_fin_shards here, so it just skips it entirely, not even touching the mutex.
   * Certainly there are tons of totally valid scenarios wherein that check will not be relevant -- a user-thread
   * exits with 1+ ever-tracked arenas still around -- but *at program exit* is a different story.  An arena-lending
   * SHM-provider (SHM-jemalloc being the first/main example, centered on jemalloc::Ipc_arena) is structured in
   * such a way as to, in the absence of something quite wacky on the user's part, pretty much assure that
   * all arenas will fully shut-down before main() exits.  And: each aforementioned dtor makes sure to
   * invoke if_requested_forget_arena_related_resources(true) (the `true` making it even more definite) before
   * dealing with stats-dumping.
   *
   * Then again, while that is decent reasoning, it is not exactly a formal proof.
   *
   * In any case it's best to not rely on separate parts of the system acting a particular way forever.  So
   * we at least defensively do it per the above logic, to avoid surprises. */
} // Finalized_shards::get_instance()

template<typename Shm_arena_t>
Finalized_shards<Shm_arena_t>::Finalized_shards() = default;

template<typename Shm_arena_t>
template<typename Task>
void Finalized_shards<Shm_arena_t>::stats_while_locked(collection_id_t collection_id, const Task& task)
{
  using flow::util::Lock_guard;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  const auto it_collection_id_and_fold_shard = m_per_arena_stats_shards.find(collection_id);
  task((it_collection_id_and_fold_shard == m_per_arena_stats_shards.end())
         ? static_cast<Sharded_stats*>(nullptr) // As advertised.  Reiterating: not an error or even unusual.
         : it_collection_id_and_fold_shard->second.get());
} // Finalized_shards::stats_while_locked()

template<typename Shm_arena_t>
void Finalized_shards<Shm_arena_t>::stats_insert(collection_id_t collection_id, const Sharded_stats& stats_shard)
{
  using flow::util::Lock_guard;
  using flow::util::stat::stats_assign;
  using flow::util::stat::stats_aggregate_one;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  auto& fold_shard = m_per_arena_stats_shards[collection_id]; // Inserts null Own<> if needed.
  if (!fold_shard)
  {
    fold_shard.reset(new Sharded_stats);

    // As advertised don't use native copying (even if it is present); use stats_assign().
    stats_assign(fold_shard.get(), stats_shard);
    return;
  }
  /* else: Fold it in.  (Why this loses nothing: see m_per_arena_stats_shards doc header.)
   *       (Also it touches any HI_WMARK fields pointlessly; but so does stats_assign().  Worth the pithiness.) */
  stats_aggregate_one(fold_shard.get(), stats_shard);
} // Finalized_shards::stats_insert()

template<typename Shm_arena_t>
void Finalized_shards<Shm_arena_t>::stats_erase(collection_id_t collection_id)
{
  using flow::util::Lock_guard;

  Lock_guard<decltype(m_mutex)> lock{m_mutex};
  m_per_arena_stats_shards.erase(collection_id); // No-op is no error, nor is it unusual.
}

} // namespace ipc::shm::arena_lend::detail::stat
