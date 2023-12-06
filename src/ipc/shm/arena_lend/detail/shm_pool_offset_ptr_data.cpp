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
// XXX
boost::movelib::unique_ptr<boost::interprocess::named_mutex>
  Shm_pool_offset_ptr_data_base::s_pool_id_mutex_or_none;

// Implementations.

Shm_pool_offset_ptr_data_base::pool_id_t Shm_pool_offset_ptr_data_base::generate_pool_id() // Static.
{
  using std::call_once;
  using Shm_obj = decltype(s_pool_id_shm_obj_or_none);
  using Shm_region = decltype(s_pool_id_shm_region_or_none);

  /* Please read our doc header for key background.  Then come back here.
   *
   * We need unique (across all processes) 31-bit IDs.  For this we use a tiny SHM region, which is not managed
   * (no memory allocation) but rather stores only the next ID. */

  /* For performance open the SHM region once and leave it open.  However this needs to happen no more than once,
   * for which we use call_once() which will likely use a mutex; we don't anticipate pool creation to be so frequent
   * as to introduce serious lock contention, especially since the critical section here is tiny. */
  call_once(s_pool_id_shm_region_init_flag, []()
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
    //XXX using std::atomic;

    Config std_log_config(Sev::S_WARNING); // Errors only.  (Only errors would appear anyway as of now but still.)
    std_log_config.init_component_to_union_idx_mapping<Log_component>(1000, 999);
    std_log_config.init_component_names<Log_component>(S_IPC_LOG_COMPONENT_NAME_MAP, false, "ipc-");

    Simple_ostream_logger std_logger(&std_log_config); // Go to default (cout, cerr).  Only cerr in practice.
    FLOW_LOG_SET_CONTEXT(&std_logger, Log_component::S_SHM);

    // The bipc SHM manip procedures can throw.
    Error_code err_code;
    op_with_possible_bipc_exception(&std_logger, &err_code, classic::error::Code::S_SHM_BIPC_MISC_LIBRARY_ERROR,
                                    "generate_pool_id().init",
                                    [&]()
    {
      /* We want unmanaged SHM which even in bipc involves a couple objects (both shall stay alive).
       * First the SHM object (pool): Either open it or create it (whoever gets to the latter first -- atomically). */

      auto little_pool_name = build_conventional_non_session_based_shared_name(Shared_name::S_RESOURCE_TYPE_ID_SHM);
      auto mutex_name = build_conventional_non_session_based_shared_name(Shared_name::S_RESOURCE_TYPE_ID_MUTEX);
      const auto suffix = Shared_name::ct("shm") / "arenaLend" / "nextPoolId";
      little_pool_name /= suffix;
      mutex_name /= suffix;

      permissions perms;
      perms.set_unrestricted(); // We have no choice given need to do cross-app writing.  @todo Provide configure knobs?
      s_pool_id_shm_obj_or_none // Currently default-cted.
        = Shm_obj(util::OPEN_OR_CREATE, little_pool_name.native_str(), bipc::read_write, perms);
      // It threw if failed.

      s_pool_id_mutex_or_none
        = boost::movelib::make_unique<boost::interprocess::named_mutex>
            (util::OPEN_OR_CREATE, mutex_name.native_str(), perms);
      // It threw if failed.

      // Set size; or no-op depending on who wins race to this call.
      s_pool_id_shm_obj_or_none.truncate(sizeof(pool_id_t));
      // It threw if failed.  Otherwise: it either zeroed the pool_id_t; or it did nothing; and now it is accessible.

      // Next get local vaddr area of the same small size.
      s_pool_id_shm_region_or_none = Shm_region(s_pool_id_shm_obj_or_none, bipc::read_write, sizeof(pool_id_t));
      // It threw if failed.
      //XXXstatic_assert(sizeof(atomic<pool_id_t>) == sizeof(pool_id_t), "Basic assumptions about atomic<> violated.");
    });

    if (err_code) // It logged details already.
    {
      FLOW_LOG_FATAL("Due to above shocking error at making a tiny SHM pool+mapping when trying to handle "
                     "SHM pool ID creation, we abort.");
      std::abort();
      // @todo We could be nicer about errors -- some error reporting to higher level.  May or may not be worth it.
    }
    // else

    // XXX
    std::cout << "XXX000: opened mini-pool [" << s_pool_id_shm_region_or_none.get_address() << "]\n"; 
  }); // call_once(init)
  /* We possibly zero-initialized s_pool_id_shm_region_or_none memory area (or no-oped due to its not needing it,
   * with a competing truncate() winning; or didn't even enter the lambda given to call_once(), as we've already
   * initialized in this process); and regardless can now read/write it. */

  /* We store the next-ID as the only thing in this little pool.  This is subtler than the following code
   * looks however.  The algorithm is conceptually obvious: On creation the truncate()d SHM area is zero-filled;
   * and atomic ++ will atomically yield the proper thing.  However we do not actually construct the atomic<>
   * at any point: we rely on this being validly constructed by merely being loaded with 0 bits.
   * I believe formally speaking the standard doesn't guarantee this; however we do loads of architecture-specific
   * stuff in Shm_pool_offset_ptr_data, so this is hardly breaking any new ground on that front.  Therefore
   * I looked at how atomic<> construction works in x86_64 gcc-9 (and by nature of how lock-free atomic<> works
   * it is exceedingly unlikely any other compiler in supported architecture(s) would do it any differently);
   * and the answer is:
   *   - Construction really does simply load the requested initial bits (we'd want 0 in our case) into the
   *     memory location.
   *   - It is only storing (such as ++) after construction where special instructions are involved.
   * Therefore: At the time we reach the ++ statement, one way or another, the memory location either has 0, or
   * it has some other value; and the ++ will perform an atomic-store-and-read instruction that races properly
   * against potential other such operations trying to do the same.  So it is safe in practice.
   *
   * Caution: With other architectures that may not be the case; would require investigation when/if we support
   * more.  Like who knows what ARM's deal is?  That said an alternative would be to use a partnered-up
   * named interprocess mutex (from bipc also; ipc::session uses one for the CNS/PID file for example) using it
   * to synchronize access to an initialized? flag and a regular pool_id_t without any atomic<>.  We could also avoid
   * any caveat (however minor) with the below do/while() loop.  However it would
   * be slower and more complex with more entropy/possibility of failure (with a mutex involved and all).  So ideally
   * keep it like this. XXX*/
  pool_id_t id;
//XXX  do
  {
    boost::interprocess::scoped_lock<boost::interprocess::named_mutex> sh_lock(*s_pool_id_mutex_or_none);
    
    // Get the next ID; and zero the MSB (reserved for the selector), leaving the proper-width next pool ID.
    //id = ((++(*(static_cast<atomic<pool_id_t>*>(s_pool_id_shm_region_or_none.get_address())))) << 1 >> 1);
    auto& id_ref = *(static_cast<pool_id_t*>(s_pool_id_shm_region_or_none.get_address()));
    id = (++id_ref) << 1 >> 1;

    std::cout << "XXX001: from mini-pool [" << s_pool_id_shm_region_or_none.get_address() << "]\n"; 
    std::cout << "XXX001Y: generated [" << id << "]\n"; 

    if (id == 0)
    {
      id = (++id_ref) << 1 >> 1;
      assert((id == 1) && "(X00...0 + 1) must be (X00...1).");

      std::cout << "!!!XXX001: generated [" << id << "]\n"; 
    }
  }
  //while (id == 0); // Formally it needs to be a loop; competitors could keep lapping us.  :-)
  /* XXX Overwhelmingly likely the while() will only iterate once.  Certainly the first time we'll yield 1, avoiding 0
   * as required (it is a reserved value for important reasons explained elsewhere).  However, as also explained
   * in this method's doc header, we do technically allow for wrapping around back to 0 (note that this would occur
   * before the full 32-bit atomic<> fully wraps around, meaning when it goes from 00...0 to 01...1 to 10...0; and
   * then a 2nd time when it goes from 11...1 to 0...0).  If that occurs, we need to skip the 0.
   * (@todo Maybe WARNING if this shockingly unlikely thing does get observed?)
   * Technically it's not quite atomic, if that actually happens (possibly another process could go 0 -> 1 or even
   * higher before our do/while() wraps comes back around for another try); but so what?  An ID value might go unused;
   * it's fine (if a bit aesthetically displeasing). */

  return id;
} // Shm_pool_offset_ptr_data_base::generate_pool_id()

} // namespace ipc::shm::arena_lend::detail
