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

#pragma once

#include <gtest/gtest.h>
#include <unordered_map>
#include "ipc/shm/arena_lend/owner_shm_pool_collection.hpp"
#include "ipc/shm/arena_lend/shm_pool.hpp"

namespace ipc::shm::arena_lend::test
{

/**
 * Listener for events of the Owner_shm_pool_collection class. In particular, it captures the last notifications,
 * if any.
 */
class Test_event_listener :
  public Owner_shm_pool_collection::Event_listener
{
public:
  /**
   * Holder of data for create notifications.
   */
  class Create_notification
  {
  public:
    /**
     * Constructor.
     *
     * @param shm_pool The shared memory pool that was in the notification.
     */
    Create_notification(const std::shared_ptr<Shm_pool>& shm_pool) :
      m_shm_pool(shm_pool)
    {
    }

    /**
     * Returns the shared memory pool of the notification.
     *
     * @return See above.
     */
    inline const std::shared_ptr<Shm_pool>& get_shm_pool() const
    {
      return m_shm_pool;
    }

  private:
    /// The shared memory pool.
    std::shared_ptr<Shm_pool> m_shm_pool;
  }; // class Create_notification

  /**
   * Holder of data for remove notifications.
   */
  class Remove_notification :
    public Create_notification
  {
  public:
    /**
     * Constructor.
     *
     * @param shm_pool The shared memory pool that was in the notification.
     * @param removed_shared_memory Whethere the shared memory pool was removed.
     */
    Remove_notification(const std::shared_ptr<Shm_pool>& shm_pool, bool removed_shared_memory) :
      Create_notification(shm_pool),
      m_removed_shared_memory(removed_shared_memory)
    {
    }

    /**
     * Returns whether shared memory was removed.
     *
     * @return See above.
     */
    inline bool get_removed_shared_memory() const
    {
      return m_removed_shared_memory;
    }

  private:
    /// Whether shared memory was removed.
    bool m_removed_shared_memory;
  }; // class Remove_notification

  /**
   * Stores the notification data for a creation event.
   *
   * @param shm_pool The shared memory pool that was created.
   */
  virtual void notify_created_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool) override
  {
    ++m_num_create_notifications;
    m_create_notification = std::make_shared<Create_notification>(shm_pool);
    auto iter = m_shared_memory_map.emplace(make_pair(shm_pool->get_name(), shm_pool));
    EXPECT_TRUE(iter.second);
  }

  /**
   * Stores the notification data for a deletion event.
   *
   * @param shm_pool The shared memory pool that was removed.
   * @param removed_shared_memory Whether the underlying shared memory was actually removed.
   */
  virtual void notify_removed_shm_pool(const std::shared_ptr<Shm_pool>& shm_pool,
                                       bool removed_shared_memory) override
  {
    ++m_num_remove_notifications;
    m_remove_notification = std::make_shared<Remove_notification>(shm_pool, removed_shared_memory);
    if (removed_shared_memory)
    {
      EXPECT_TRUE(m_shared_memory_map.erase(shm_pool->get_name()) > 0);
    }
  }

  /**
   * Returns the last notification for a creation event.
   *
   * @return See above.
   */
  inline std::shared_ptr<Create_notification> get_create_notification() const
  {
    return m_create_notification;
  }

  /**
   * Returns the number of creation event notifications since last reset.
   *
   * @return See above.
   */
  inline unsigned int get_num_create_notifications() const
  {
    return m_num_create_notifications;
  }

  /**
   * Returns the last notification for a deletion event.
   *
   * @return See above.
   */
  inline std::shared_ptr<Remove_notification> get_remove_notification() const
  {
    return m_remove_notification;
  }

  /**
   * Returns the number of deletion event notifications since last reset.
   *
   * @return See above.
   */
  inline unsigned int get_num_remove_notifications() const
  {
    return m_num_remove_notifications;
  }

  /**
   * Clears the notifications.
   */
  inline void reset_notifications()
  {
    m_num_create_notifications = 0;
    m_create_notification = nullptr;
    m_num_remove_notifications = 0;
    m_remove_notification = nullptr;
  }

  /**
   * Returns the quantity of shared memory objects that have not been removed.
   *
   * @return See above.
   */
  inline std::size_t get_num_shared_memory_objects() const
  {
    return m_shared_memory_map.size();
  }

  /**
   * Prints the shared memory objects to a stream.
   *
   * @param os The stream to output to.
   *
   * @return The stream.
   */
  std::ostream& print_shared_memory_objects(std::ostream& os) const
  {
    if (m_shared_memory_map.empty())
    {
      os << "No shared memory objects";
    }

    for (const auto& iter : m_shared_memory_map)
    {
      os << *iter.second;
    }

    return os;
  }

private:
  /// The number of create notifications since last reset.
  unsigned int m_num_create_notifications = 0;
  /// The last notification for a creation event.
  std::shared_ptr<Create_notification> m_create_notification;
  /// The number of remove notifications since last reset.
  unsigned int m_num_remove_notifications = 0;
  /// The last notification for a removal event.
  std::shared_ptr<Remove_notification> m_remove_notification;
  /// Tracks shared memory pools.
  std::unordered_map<std::string, std::shared_ptr<Shm_pool>> m_shared_memory_map;
}; // class Test_event_listener

} // namespace ipc::shm::arena_lend::test
