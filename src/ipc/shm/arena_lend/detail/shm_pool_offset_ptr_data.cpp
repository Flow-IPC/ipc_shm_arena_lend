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
#include "ipc/shm/arena_lend/detail/shm_pool_offset_ptr_data.hpp"
#include "ipc/session/detail/session_shared_name.hpp"
#include "ipc/util/util_fwd.hpp"
#include "ipc/util/detail/util.hpp"
#include "ipc/shm/classic/error.hpp"
#include <flow/log/simple_ostream_logger.hpp>
#include <boost/interprocess/permissions.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

namespace ipc::shm::arena_lend::detail
{

// Static initializers.

std::once_flag Shm_pool_offset_ptr_data_base::s_pool_id_shm_region_init_flag;
bipc::shared_memory_object Shm_pool_offset_ptr_data_base::s_pool_id_shm_obj_or_none;
bipc::mapped_region Shm_pool_offset_ptr_data_base::s_pool_id_shm_region_or_none;
std::optional<boost::interprocess::named_mutex> Shm_pool_offset_ptr_data_base::s_pool_id_mutex_or_none;

// Implementations.

Shm_pool_offset_ptr_data_base::pool_id_t Shm_pool_offset_ptr_data_base::generate_pool_id() // Static.
{
  using std::call_once;
  using Shm_obj = decltype(s_pool_id_shm_obj_or_none);
  using Shm_region = decltype(s_pool_id_shm_region_or_none);
  using Sh_lock = bipc::scoped_lock<bipc::named_mutex>;

  /* Please read our doc header for key background.  Then come back here.
   *
   * We need unique (across all processes) 31-bit IDs.  For this we use a tiny SHM region, which is not managed
   * (no memory allocation) but rather stores only the next ID. */

  pool_id_t id = 0;

  /* For performance open the SHM region once and leave it open.  However this needs to happen no more than once,
   * for which we use call_once() which will likely use a mutex; we don't anticipate pool creation to be so frequent
   * as to introduce serious lock contention, especially since the critical section here is usually tiny. */
  call_once(s_pool_id_shm_region_init_flag, [&]()
  {
    /* We don't really need to log, normally, but if something goes wrong, it'd be good to at least log to standard
     * error (even if it gets interleaves with something) -- we're gonna abort() thereafter.  So set up a
     * make-shift console logger. */
    using util::build_conventional_non_session_based_shared_name;
    using util::op_with_possible_bipc_exception;
    using util::Shared_name;
    using flow::log::Simple_ostream_logger;
    using flow::log::Config;
    using flow::log::Sev;
    using bipc::permissions;
    using bipc::interprocess_exception;

    Config std_log_config(Sev::S_WARNING); // Errors only.  (Only errors would appear anyway as of now but still.)
    std_log_config.init_component_to_union_idx_mapping<Log_component>(1000, 999);
    std_log_config.init_component_names<Log_component>(S_IPC_LOG_COMPONENT_NAME_MAP, false, "ipc-");

    Simple_ostream_logger std_logger(&std_log_config); // Go to default (cout, cerr).  Only cerr in practice.
    FLOW_LOG_SET_CONTEXT(&std_logger, Log_component::S_SHM);

    /* The post-condition of this call_once():
     *   - s_pool_id_shm_region_or_none.get_address() = mapping of opened handle to the SHM object (pool).
     *   - That pool is properly sized and contains the last-used ID (so we can ++ that and return copy thereof).
     *
     * If call_once() is called not-the-1st-time, then we've already guaranteed it before in this process.
     * If called the 1st time, then we are here (where you're reading) and must ensure the above post-condition.
     * Then read on:
     *
     * There are two possibilities: The SHM object (pool) does not yet exist system-wide; then we must get it to
     * the above state: create it; size it; and set ID inside to initial value zero.  Or: it does exist; then we simply
     * open it.  However in the former case:
     *   - Those 3 steps must be done atomically versus other processes executing identical code (the call_once()
     *     ensures it won't be other threads in *this* process).  Hence why named-mutex *s_pool_id_mutex_or_none
     *     protects it, so we must lock it around this code.
     *
     * In addition we add an optimization: In the have-to-create-pool case we'd otherwise fill-out the ID to 0 (invalid)
     * and then ourselves in the next section (after the call_once()) re-lock mutex, ++ it, and return copy.
     * However since we ourselves are initializing the ID to 0, we might as well do the ++ in one swoop: hence
     * simply set it to 1 immediately and return 1; thus short-circuiting and returning before
     * the post-call_once() section.
     *
     * So now let's see whether pool exists yet or not.  The only way to do it is try opening it and react
     * if already exists.  Note that any attempt to use OPEN_OR_CREATE does not suit us: then we cannot tell if
     * we opened or created it, hence we don't know whether we must initialize its size and contents (the latter
     * being the dangerous part; setting the size would just be redundant but not harmful).  Note that the
     * "physical" memory after SHM object creation is not zeroed consistently across processes (even though
     * it might appear that way); so it is not sufficient to just create the object and then ++ the memory, thinking
     * it is zero: It'll "be" zero in process A... but possibly zero in process B around the same time; chaos results.
     * (This comes from experience.  Nasty sporadic bugs have resulted from assuming the memory is zeroed from
     * the get-do.)  So: we do need to OPEN_ONLY and then CREATE_ONLY if necessary -- separately. */

    const auto SUFFIX = Shared_name::ct("shm") / "arenaLend" / "nextPoolId";
    auto little_pool_name = build_conventional_non_session_based_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM);
    auto mutex_name = build_conventional_non_session_based_shared_name(Shared_name::S_RESOURCE_TYPE_ID_MUTEX);
    little_pool_name /= SUFFIX;
    mutex_name /= SUFFIX;

    permissions perms;
    perms.set_unrestricted(); // We have no choice given need to do cross-app writing.  @todo Provide configure knobs?

    // The bipc SHM manip procedures can throw.  Spoiler alert: if that happens sans remedy => kaput/abort program.
    Error_code err_code;
    op_with_possible_bipc_exception(&std_logger, &err_code, classic::error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR,
                                    "generate_pool_id().init",
                                    [&]()
    {
      /* The mutex (prob. semaphore) itself needs to be created-or-opened atomically.  Luckily there are no
       * initialization problems with these guys (or we'd really be screwed). */
      s_pool_id_mutex_or_none.emplace(util::OPEN_OR_CREATE, mutex_name.native_str(), perms);
      // It threw if failed.

      // Okay, now we can lock it as noted above; and we will reuse this handle to it from now on in this process.

      {
        Sh_lock sh_lock(*s_pool_id_mutex_or_none);

        // Let's see if indeed pool already exists.
        try
        {
          s_pool_id_shm_obj_or_none // Currently default-cted.
            = Shm_obj(util::OPEN_ONLY, little_pool_name.native_str(), bipc::read_write);

          // Didn't throw?  Then pool already existed and was therefore initialized by someone else; we have a handle.
        }
        catch (const boost::interprocess::interprocess_exception& exc) // Threw?  Then:
        {
          /* Probably it failed due to not-existing; that would be normal: we won the race to here among processes.
           * Otherwise rethrow and let the error handling below deal with it (spoiler alert: catastrophic). */
          if (exc.get_error_code() != boost::interprocess::not_found_error)
          {
            throw;
          }
          // else: Indeed, not found.  Must create underlying pool.

          s_pool_id_shm_obj_or_none // Currently default-cted (still).
            = Shm_obj(util::CREATE_ONLY, little_pool_name.native_str(), bipc::read_write, perms);
          // It threw if failed.

          /* We were the ones to create it; so we must initialize it for everyone before unlocking.  Mutex
           * is still locked; it protects both the existence-while-initialized-or-non-existence of the SHM-pool
           * across the system; and the value stored inside it.  Or simply put it protects its entire state. */

          s_pool_id_shm_obj_or_none.truncate(sizeof(pool_id_t));
          // It threw if failed.

          // To actually initialize it we must map a local vaddr area to it,
          s_pool_id_shm_region_or_none = Shm_region(s_pool_id_shm_obj_or_none, bipc::read_write, sizeof(pool_id_t));
          // It threw if failed.

          // Lastly start the ID in this deterministic initial state.  (Only ++ after this, in all processes.)
          id = *(static_cast<pool_id_t*>(s_pool_id_shm_region_or_none.get_address())) = 1;

          return; // id != 0 => we return from method next.

          /* Alternatively we could have set it to 0 and not returned yet, letting the general case below
           * ++ it (maybe to 1, or maybe higher if another guy is racing us and manages to ++ it before we
           * re-lock the mutex below).  (Except s_pool_id_shm_region_or_none would not need to be assigned,
           * as we had to do that above.)  This way is just more performant and arguably simpler
           * to think about. */
        } // catch () // From Shm_obj() ctor.
        // Got here: catch(){} body did not execute; pool handle opened fine.
      } // Sh_lock sh_lock(*s_pool_id_mutex_or_none)

      // Got here: opened pool handle, and pool in solid shape, but we do still need to map it for subsequent access.
      s_pool_id_shm_region_or_none = Shm_region(s_pool_id_shm_obj_or_none, bipc::read_write, sizeof(pool_id_t));
      // It threw if failed.
    }); // op_with_possible_bipc_exception()
    if (id != 0)
    {
      return; // This is the case where we initialized `id = 1` and the pool contents accordingly.
    }
    // else

    if (err_code) // It logged details already.
    {
      FLOW_LOG_FATAL("Due to above shocking error at making a named mutex+tiny SHM pool+mapping when trying to handle "
                     "SHM pool ID creation, we abort.");
      std::abort();
      // @todo We could be nicer about errors -- some error reporting to higher level.  May or may not be worth it.
    }
    // else
  }); // call_once(init)
  if (id != 0)
  {
    return id; // This is the case where we initialized `id = 1` and the pool contents accordingly.
  }
  // else

  /* Post-condition of call_once() is now guaranteed: s_pool_id_shm_region_or_none is mapped to an active pool
   * pool that stores some valid ID.  Whether that's because this process had already made it so before, or
   * because just did so above by locking mutex/opening pool/unlocking mutex, it doesn't matter.  We shall
   * now access the pool's contents and ++ that ID, at the same time.  Hence lock mutex.
   *
   * (We could also lock mutex around all of the above and below together.  Doesn't matter too much; it only
   * makes a difference the first time per process in which case we lock/unlock/lock/unlock; but this way in
   * the not-first-time case the critical section is a bit smaller.  Though maybe the code would be simpler
   * the other way.  Not quite a to-do in my opinion (ygoldfel).) */

  {
    Sh_lock sh_lock(*s_pool_id_mutex_or_none);

    // Get the next ID; and zero the MSB (reserved for the selector), leaving the proper-width next pool ID.
    auto& id_ref = *(static_cast<pool_id_t*>(s_pool_id_shm_region_or_none.get_address()));
    id = (++id_ref) << 1 >> 1;

    if (id == 0)
    {
      ++id_ref;
      id = 1;

      assert((((id_ref << 1 >> 1) == 1)) && "(X00...0 + 1) must be (X00...1).  Is the above logic broken?");
    }
    /* Overwhelmingly likely the `if` will not execute {body}.  Certainly the first time we'll yield >1, avoiding 0
     * as required (it is a reserved value for important reasons explained elsewhere).  However, as also explained
     * in this method's doc header, we do technically allow for wrapping around back to 0 (note that this would occur
     * before the full 32-bit # fully wraps around, meaning when it goes from 00...0 to 01...1 to 10...0; and
     * then a 2nd time when it goes from 11...1 to 0...0).  If that occurs, we need to skip the 0.
     * (@todo Maybe WARNING if this shockingly unlikely thing does get observed?) */
  } // Sh_lock sh_lock(*s_pool_id_mutex_or_none)

  return id;
} // Shm_pool_offset_ptr_data_base::generate_pool_id()

} // namespace ipc::shm::arena_lend::detail
