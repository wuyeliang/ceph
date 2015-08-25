// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "test/librbd/test_fixture.h"
#include "test/librbd/test_support.h"
#include "include/int_types.h"
#include "include/stringify.h"
#include "include/rados/librados.h"
#include "include/rbd/librbd.hpp"
#include "common/Cond.h"
#include "common/errno.h"
#include "common/Mutex.h"
#include "common/RWLock.h"
#include "cls/lock/cls_lock_client.h"
#include "cls/lock/cls_lock_types.h"
#include "librbd/AioCompletion.h"
#include "librbd/AioImageRequestWQ.h"
#include "librbd/internal.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageWatcher.h"
#include "librbd/WatchNotifyTypes.h"
#include "test/librados/test.h"
#include "gtest/gtest.h"
#include <boost/assign/std/set.hpp>
#include <boost/assign/std/map.hpp>
#include <boost/bind.hpp>
#include <boost/scope_exit.hpp>
#include <boost/thread/thread.hpp>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

using namespace ceph;
using namespace boost::assign;
using namespace librbd::watch_notify;

void register_test_image_watcher() {
}

class TestImageWatcher : public TestFixture {
public:

  TestImageWatcher() : m_watch_ctx(NULL), m_callback_lock("m_callback_lock")
  {
  }

  struct LockListener : public librbd::ImageWatcher::Listener {
    Mutex lock;
    Cond cond;
    size_t releasing_lock_count;
    size_t lock_updated_count;
    bool lock_owner;

    LockListener()
      : lock("lock"), releasing_lock_count(0), lock_updated_count(0),
        lock_owner(false) {
    }

    virtual bool handle_requested_lock() {
      return true;
    }
    virtual void handle_lock_updated(
        librbd::ImageWatcher::LockUpdateState state) {
      Mutex::Locker locker(lock);
      ++lock_updated_count;

      switch (state) {
      case librbd::ImageWatcher::LOCK_UPDATE_STATE_NOT_SUPPORTED:
      case librbd::ImageWatcher::LOCK_UPDATE_STATE_UNLOCKED:
      case librbd::ImageWatcher::LOCK_UPDATE_STATE_NOTIFICATION:
        lock_owner = false;
        break;
      case librbd::ImageWatcher::LOCK_UPDATE_STATE_RELEASING:
        lock_owner = false;
        ++releasing_lock_count;
        break;
      case librbd::ImageWatcher::LOCK_UPDATE_STATE_LOCKED:
        lock_owner = true;
        break;
      }
      cond.Signal();
    }
  };

  class WatchCtx : public librados::WatchCtx2 {
  public:
    WatchCtx(TestImageWatcher &parent) : m_parent(parent), m_handle(0) {}

    int watch(const librbd::ImageCtx &ictx) {
      m_header_oid = ictx.header_oid;
      return m_parent.m_ioctx.watch2(m_header_oid, &m_handle, this);
    }

    int unwatch() {
      return m_parent.m_ioctx.unwatch2(m_handle);
    }

    virtual void handle_notify(uint64_t notify_id,
                               uint64_t cookie,
                               uint64_t notifier_id,
                               bufferlist& bl) {
      try {
	int op;
	bufferlist payload;
	bufferlist::iterator iter = bl.begin();
	DECODE_START(1, iter);
	::decode(op, iter);
	iter.copy_all(payload);
	DECODE_FINISH(iter);

        NotifyOp notify_op = static_cast<NotifyOp>(op);
        /*
	std::cout << "NOTIFY: " << notify_op << ", " << notify_id
		  << ", " << cookie << ", " << notifier_id << std::endl;
        */

	Mutex::Locker l(m_parent.m_callback_lock);
        m_parent.m_notify_payloads[notify_op] = payload;

        bufferlist reply;
        if (m_parent.m_notify_acks.count(notify_op) > 0) {
          reply = m_parent.m_notify_acks[notify_op];
	  m_parent.m_notifies += notify_op;
	  m_parent.m_callback_cond.Signal();
        }

	m_parent.m_ioctx.notify_ack(m_header_oid, notify_id, cookie, reply);
      } catch (...) {
	FAIL();
      }
    }

    virtual void handle_error(uint64_t cookie, int err) {
      std::cerr << "ERROR: " << cookie << ", " << cpp_strerror(err)
		<< std::endl; 
    }

    uint64_t get_handle() const {
      return m_handle;
    }

  private:
    TestImageWatcher &m_parent;
    std::string m_header_oid;
    uint64_t m_handle;
  };

  virtual void TearDown() {
    deregister_image_watch();
    TestFixture::TearDown();
  }

  int deregister_image_watch() {
    if (m_watch_ctx != NULL) {
      int r = m_watch_ctx->unwatch();

      librados::Rados rados(m_ioctx);
      rados.watch_flush();

      delete m_watch_ctx;
      m_watch_ctx = NULL;
      return r;
    }
    return 0;
  }

  void register_lock_listener(librbd::ImageCtx &ictx) {
    ictx.image_watcher->register_listener(&m_lock_listener);
  }

  int register_image_watch(librbd::ImageCtx &ictx) {
    m_watch_ctx = new WatchCtx(*this);
    return m_watch_ctx->watch(ictx);
  }

  bool wait_for_releasing_lock(librbd::ImageCtx &ictx) {
    Mutex::Locker locker(m_lock_listener.lock);
    while (m_lock_listener.releasing_lock_count == 0) {
      if (m_lock_listener.cond.WaitInterval(ictx.cct, m_lock_listener.lock,
                                            utime_t(10, 0)) != 0) {
        return false;
      }
    }
    m_lock_listener.releasing_lock_count = 0;
    return true;
  }

  bool wait_for_lock_updated(librbd::ImageCtx &ictx) {
    Mutex::Locker locker(m_lock_listener.lock);
    while (m_lock_listener.lock_updated_count == 0) {
      if (m_lock_listener.cond.WaitInterval(ictx.cct, m_lock_listener.lock,
                                            utime_t(10, 0)) != 0) {
        return false;
      }
    }
    m_lock_listener.lock_updated_count = 0;
    return true;
  }

  bool wait_for_notifies(librbd::ImageCtx &ictx) {
    Mutex::Locker l(m_callback_lock);
    while (m_notifies.size() < m_notify_acks.size()) {
      int r = m_callback_cond.WaitInterval(ictx.cct, m_callback_lock,
					 utime_t(10, 0));
      if (r != 0) {
	break;
      }
    }
    return (m_notifies.size() == m_notify_acks.size());
  }

  bufferlist create_response_message(int r) {
    bufferlist bl;
    ::encode(ResponseMessage(r), bl);
    return bl;
  }

  bool extract_async_request_id(NotifyOp op, AsyncRequestId *id) {
    if (m_notify_payloads.count(op) == 0) {
      return false;
    }

    bufferlist payload = m_notify_payloads[op];
    bufferlist::iterator iter = payload.begin();

    switch (op) {
    case NOTIFY_OP_FLATTEN:
      {
        FlattenPayload payload;
        payload.decode(2, iter);
        *id = payload.async_request_id;
      }
      return true;
    case NOTIFY_OP_RESIZE:
      {
        ResizePayload payload;
        payload.decode(2, iter);
        *id = payload.async_request_id;
      }
      return true;
    case NOTIFY_OP_REBUILD_OBJECT_MAP:
      {
        RebuildObjectMapPayload payload;
        payload.decode(2, iter);
        *id = payload.async_request_id;
      }
      return true;
    default:
      break;
    }
    return false;
  }

  int notify_async_progress(librbd::ImageCtx *ictx, const AsyncRequestId &id,
                            uint64_t offset, uint64_t total) {
    bufferlist bl;
    ::encode(NotifyMessage(AsyncProgressPayload(id, offset, total)), bl);
    return m_ioctx.notify2(ictx->header_oid, bl, 5000, NULL);
  }

  int notify_async_complete(librbd::ImageCtx *ictx, const AsyncRequestId &id,
                            int r) {
    bufferlist bl;
    ::encode(NotifyMessage(AsyncCompletePayload(id, r)), bl);
    return m_ioctx.notify2(ictx->header_oid, bl, 5000, NULL);
  }

  typedef std::map<NotifyOp, bufferlist> NotifyOpPayloads;
  typedef std::set<NotifyOp> NotifyOps;

  WatchCtx *m_watch_ctx;

  LockListener m_lock_listener;

  NotifyOps m_notifies;
  NotifyOpPayloads m_notify_payloads;
  NotifyOpPayloads m_notify_acks;

  AsyncRequestId m_async_request_id;

  Mutex m_callback_lock;
  Cond m_callback_cond;

};

struct ProgressContext : public librbd::ProgressContext {
  Mutex mutex;
  Cond cond;
  bool received;
  uint64_t offset;
  uint64_t total;

  ProgressContext() : mutex("ProgressContext::mutex"), received(false),
                      offset(0), total(0) {}

  virtual int update_progress(uint64_t offset_, uint64_t total_) {
    Mutex::Locker l(mutex);
    offset = offset_;
    total = total_;
    received = true;
    cond.Signal();
    return 0;
  }

  bool wait(librbd::ImageCtx *ictx, uint64_t offset_, uint64_t total_) {
    Mutex::Locker l(mutex);
    while (!received) {
      int r = cond.WaitInterval(ictx->cct, mutex, utime_t(10, 0));
      if (r != 0) {
	break;
      }
    }
    return (received && offset == offset_ && total == total_);
  }
};

struct FlattenTask {
  librbd::ImageCtx *ictx;
  ProgressContext *progress_context;
  int result;

  FlattenTask(librbd::ImageCtx *ictx_, ProgressContext *ctx)
    : ictx(ictx_), progress_context(ctx), result(0) {}

  void operator()() {
    RWLock::RLocker l(ictx->owner_lock);
    result = ictx->image_watcher->notify_flatten(0, *progress_context);
  }
};

struct ResizeTask {
  librbd::ImageCtx *ictx;
  ProgressContext *progress_context;
  int result;

  ResizeTask(librbd::ImageCtx *ictx_, ProgressContext *ctx)
    : ictx(ictx_), progress_context(ctx), result(0) {}

  void operator()() {
    RWLock::RLocker l(ictx->owner_lock);
    result = ictx->image_watcher->notify_resize(0, 0, *progress_context);
  }
};

struct RebuildObjectMapTask {
  librbd::ImageCtx *ictx;
  ProgressContext *progress_context;
  int result;

  RebuildObjectMapTask(librbd::ImageCtx *ictx_, ProgressContext *ctx)
    : ictx(ictx_), progress_context(ctx), result(0) {}

  void operator()() {
    RWLock::RLocker l(ictx->owner_lock);
    result = ictx->image_watcher->notify_rebuild_object_map(0, *progress_context);
  }
};

TEST_F(TestImageWatcher, IsLockSupported) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  RWLock::WLocker l(ictx->owner_lock);
  ASSERT_TRUE(ictx->image_watcher);
  ASSERT_TRUE(ictx->image_watcher->is_lock_supported());

  ictx->read_only = true;
  ASSERT_FALSE(ictx->image_watcher->is_lock_supported());
  ictx->read_only = false;

  ictx->features &= ~RBD_FEATURE_EXCLUSIVE_LOCK;
  ASSERT_FALSE(ictx->image_watcher->is_lock_supported());
  ictx->features |= RBD_FEATURE_EXCLUSIVE_LOCK;

  ictx->snap_id = 1234;
  ASSERT_FALSE(ictx->image_watcher->is_lock_supported());
  ictx->snap_id = CEPH_NOSNAP;
}

TEST_F(TestImageWatcher, TryLock) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_TRUE(ictx->image_watcher);

  {
    RWLock::WLocker l(ictx->owner_lock);
    ASSERT_EQ(0, ictx->image_watcher->try_lock());
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }

  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t> lockers;
  ClsLockType lock_type;
  ASSERT_EQ(0, rados::cls::lock::get_lock_info(&m_ioctx, ictx->header_oid,
					       RBD_LOCK_NAME, &lockers,
					       &lock_type, NULL));
  ASSERT_EQ(LOCK_EXCLUSIVE, lock_type);
  ASSERT_EQ(1U, lockers.size());
}

TEST_F(TestImageWatcher, TryLockNotifyAnnounceLocked) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};

  {
    RWLock::WLocker l(ictx->owner_lock);
    ASSERT_EQ(0, ictx->image_watcher->try_lock());
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_ACQUIRED_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, TryLockWithTimedOutOwner) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  // use new Rados connection due to blacklisting
  librados::Rados rados;
  ASSERT_EQ("", connect_cluster_pp(rados));

  librados::IoCtx io_ctx;
  ASSERT_EQ(0, rados.ioctx_create(_pool_name.c_str(), io_ctx));
  librbd::ImageCtx *ictx = new librbd::ImageCtx(m_image_name.c_str(), "", NULL,
					        io_ctx, false);
  ASSERT_EQ(0, librbd::open_image(ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE, "auto 1234"));
  librbd::close_image(ictx);
  io_ctx.close();

  // no watcher on the locked image means we can break the lock
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  RWLock::WLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->try_lock());
  ASSERT_TRUE(ictx->image_watcher->is_lock_owner());

  rados.test_blacklist_self(false);
}

TEST_F(TestImageWatcher, TryLockWithUserExclusiveLock) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE, "manually locked"));

  RWLock::WLocker l(ictx->owner_lock);
  ASSERT_EQ(-EBUSY, ictx->image_watcher->try_lock());
  ASSERT_FALSE(ictx->image_watcher->is_lock_owner());

  ASSERT_EQ(0, unlock_image());
  ASSERT_EQ(0, ictx->image_watcher->try_lock());
  ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
}

TEST_F(TestImageWatcher, TryLockWithUserSharedLocked) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, lock_image(*ictx, LOCK_SHARED, "manually locked"));

  RWLock::WLocker l(ictx->owner_lock);
  ASSERT_EQ(-EBUSY, ictx->image_watcher->try_lock());
  ASSERT_FALSE(ictx->image_watcher->is_lock_owner());

  ASSERT_EQ(0, unlock_image());
  ASSERT_EQ(0, ictx->image_watcher->try_lock());
  ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
}

TEST_F(TestImageWatcher, ReleaseLockNotLocked) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  RWLock::WLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->release_lock());
}

TEST_F(TestImageWatcher, ReleaseLockNotifies) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};

  {
    RWLock::WLocker l(ictx->owner_lock);
    ASSERT_EQ(0, ictx->image_watcher->try_lock());
  }
  ASSERT_TRUE(wait_for_notifies(*ictx));

  m_notify_acks += std::make_pair(NOTIFY_OP_RELEASED_LOCK, bufferlist());
  {
    RWLock::WLocker l(ictx->owner_lock);
    ASSERT_EQ(0, ictx->image_watcher->release_lock());
  }
  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_ACQUIRED_LOCK, NOTIFY_OP_RELEASED_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, ReleaseLockBrokenLock) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  RWLock::WLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->try_lock());

  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t> lockers;
  ClsLockType lock_type;
  ASSERT_EQ(0, rados::cls::lock::get_lock_info(&m_ioctx, ictx->header_oid,
                                               RBD_LOCK_NAME, &lockers,
                                               &lock_type, NULL));
  ASSERT_EQ(1U, lockers.size());
  ASSERT_EQ(0, rados::cls::lock::break_lock(&m_ioctx, ictx->header_oid,
					    RBD_LOCK_NAME,
					    lockers.begin()->first.cookie,
					    lockers.begin()->first.locker));

  ASSERT_EQ(0, ictx->image_watcher->release_lock());
}

TEST_F(TestImageWatcher, RequestLock) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_ACQUIRED_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }
}

TEST_F(TestImageWatcher, RequestLockFromPeer) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
			  "auto " + stringify(m_watch_ctx->get_handle())));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_REQUEST_LOCK, create_response_message(0)}};

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_REQUEST_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  ASSERT_EQ(0, unlock_image());

  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_RELEASED_LOCK,{}}};
  }

  bufferlist bl;
  {
    ENCODE_START(1, 1, bl);
    ::encode(NOTIFY_OP_RELEASED_LOCK, bl);
    ENCODE_FINISH(bl);
  }
  ASSERT_EQ(0, m_ioctx.notify2(ictx->header_oid, bl, 5000, NULL));
  ASSERT_TRUE(wait_for_lock_updated(*ictx));

  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};
  }

  {
    RWLock::RLocker owner_lock(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  expected_notify_ops.clear();
  expected_notify_ops += NOTIFY_OP_ACQUIRED_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }
}

TEST_F(TestImageWatcher, RequestLockTimedOut) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
			  "auto " + stringify(m_watch_ctx->get_handle())));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_REQUEST_LOCK, {}}};

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_REQUEST_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  // should resend when empty ack returned
  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
  }
  ASSERT_TRUE(wait_for_notifies(*ictx));

  {
    Mutex::Locker l(m_callback_lock);
    ASSERT_EQ(0, unlock_image());
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  ASSERT_TRUE(wait_for_lock_updated(*ictx));

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }
}

TEST_F(TestImageWatcher, RequestLockIgnored) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
			  "auto " + stringify(m_watch_ctx->get_handle())));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_REQUEST_LOCK, create_response_message(0)}};

  int orig_notify_timeout = ictx->cct->_conf->client_notify_timeout;
  ictx->cct->_conf->set_val("client_notify_timeout", "0");
  BOOST_SCOPE_EXIT( (ictx)(orig_notify_timeout) ) {
    ictx->cct->_conf->set_val("client_notify_timeout",
                              stringify(orig_notify_timeout));
  } BOOST_SCOPE_EXIT_END;

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_REQUEST_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  // after the request times out -- it will be resent
  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
  }
  ASSERT_TRUE(wait_for_notifies(*ictx));
  ASSERT_EQ(expected_notify_ops, m_notifies);

  {
    Mutex::Locker l(m_callback_lock);
    ASSERT_EQ(0, unlock_image());
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  ASSERT_TRUE(wait_for_lock_updated(*ictx));

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }
}

TEST_F(TestImageWatcher, RequestLockTryLockRace) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
                          "auto " + stringify(m_watch_ctx->get_handle())));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_REQUEST_LOCK, create_response_message(0)}};

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_REQUEST_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_RELEASED_LOCK, {}}};
  }

  bufferlist bl;
  {
    ENCODE_START(1, 1, bl);
    ::encode(NOTIFY_OP_RELEASED_LOCK, bl);
    ENCODE_FINISH(bl);
  }
  ASSERT_EQ(0, m_ioctx.notify2(ictx->header_oid, bl, 5000, NULL));

  // after losing race -- it will re-request
  ASSERT_TRUE(wait_for_notifies(*ictx));

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_FALSE(ictx->image_watcher->is_lock_owner());
  }

  {
    Mutex::Locker l(m_callback_lock);
    ASSERT_EQ(0, unlock_image());
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_RELEASED_LOCK, {}}};
  }

  ASSERT_EQ(0, m_ioctx.notify2(ictx->header_oid, bl, 5000, NULL));
  ASSERT_TRUE(wait_for_lock_updated(*ictx));

  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};
  }

  {
    RWLock::RLocker owner_lock(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_lock_updated(*ictx));
  ASSERT_TRUE(wait_for_notifies(*ictx));

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }
}

TEST_F(TestImageWatcher, RequestLockTryLockFailed) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_SHARED, "manually 1234"));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_REQUEST_LOCK, {}}};

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_REQUEST_LOCK;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  // should resend when error encountered
  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
  }
  ASSERT_TRUE(wait_for_notifies(*ictx));

  {
    Mutex::Locker l(m_callback_lock);
    ASSERT_EQ(0, unlock_image());
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));
}

TEST_F(TestImageWatcher, NotifyHeaderUpdate) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));

  m_notify_acks = {{NOTIFY_OP_HEADER_UPDATE, {}}};
  librbd::ImageWatcher::notify_header_update(m_ioctx, ictx->header_oid);

  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_HEADER_UPDATE;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifyFlatten) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_FLATTEN, create_response_message(0)}};

  ProgressContext progress_context;
  FlattenTask flatten_task(ictx, &progress_context);
  boost::thread thread(boost::ref(flatten_task));

  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_FLATTEN;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  AsyncRequestId async_request_id;
  ASSERT_TRUE(extract_async_request_id(NOTIFY_OP_FLATTEN, &async_request_id));

  ASSERT_EQ(0, notify_async_progress(ictx, async_request_id, 10, 20));
  ASSERT_TRUE(progress_context.wait(ictx, 10, 20));

  ASSERT_EQ(0, notify_async_complete(ictx, async_request_id, 0));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(0, flatten_task.result);
}

TEST_F(TestImageWatcher, NotifyResize) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_RESIZE, create_response_message(0)}};

  ProgressContext progress_context;
  ResizeTask resize_task(ictx, &progress_context);
  boost::thread thread(boost::ref(resize_task));

  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_RESIZE;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  AsyncRequestId async_request_id;
  ASSERT_TRUE(extract_async_request_id(NOTIFY_OP_RESIZE, &async_request_id));

  ASSERT_EQ(0, notify_async_progress(ictx, async_request_id, 10, 20));
  ASSERT_TRUE(progress_context.wait(ictx, 10, 20));

  ASSERT_EQ(0, notify_async_complete(ictx, async_request_id, 0));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(0, resize_task.result);
}

TEST_F(TestImageWatcher, NotifyRebuildObjectMap) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_REBUILD_OBJECT_MAP, create_response_message(0)}};

  ProgressContext progress_context;
  RebuildObjectMapTask rebuild_task(ictx, &progress_context);
  boost::thread thread(boost::ref(rebuild_task));

  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_REBUILD_OBJECT_MAP;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  AsyncRequestId async_request_id;
  ASSERT_TRUE(extract_async_request_id(NOTIFY_OP_REBUILD_OBJECT_MAP,
                                       &async_request_id));

  ASSERT_EQ(0, notify_async_progress(ictx, async_request_id, 10, 20));
  ASSERT_TRUE(progress_context.wait(ictx, 10, 20));

  ASSERT_EQ(0, notify_async_complete(ictx, async_request_id, 0));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(0, rebuild_task.result);
}

TEST_F(TestImageWatcher, NotifySnapCreate) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_CREATE, create_response_message(0)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->notify_snap_create("snap"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_CREATE;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifySnapCreateError) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_CREATE, create_response_message(-EEXIST)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(-EEXIST, ictx->image_watcher->notify_snap_create("snap"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_CREATE;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifySnapRename) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_RENAME, create_response_message(0)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->notify_snap_rename(1, "snap-rename"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_RENAME;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifySnapRenameError) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_RENAME, create_response_message(-EEXIST)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(-EEXIST, ictx->image_watcher->notify_snap_rename(1, "snap-rename"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_RENAME;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifySnapRemove) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_REMOVE, create_response_message(0)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->notify_snap_remove("snap"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_REMOVE;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifySnapProtect) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_PROTECT, create_response_message(0)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->notify_snap_protect("snap"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_PROTECT;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifySnapUnprotect) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_SNAP_UNPROTECT, create_response_message(0)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->notify_snap_unprotect("snap"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_SNAP_UNPROTECT;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifyRename) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_RENAME, create_response_message(0)}};

  RWLock::RLocker l(ictx->owner_lock);
  ASSERT_EQ(0, ictx->image_watcher->notify_rename("new_name"));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_RENAME;
  ASSERT_EQ(expected_notify_ops, m_notifies);
}

TEST_F(TestImageWatcher, NotifyAsyncTimedOut) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_FLATTEN, {}}};

  ProgressContext progress_context;
  FlattenTask flatten_task(ictx, &progress_context);
  boost::thread thread(boost::ref(flatten_task));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(-ETIMEDOUT, flatten_task.result);
}

TEST_F(TestImageWatcher, NotifyAsyncError) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_FLATTEN, create_response_message(-EIO)}};

  ProgressContext progress_context;
  FlattenTask flatten_task(ictx, &progress_context);
  boost::thread thread(boost::ref(flatten_task));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(-EIO, flatten_task.result);
}

TEST_F(TestImageWatcher, NotifyAsyncCompleteError) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
        "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_FLATTEN, create_response_message(0)}};

  ProgressContext progress_context;
  FlattenTask flatten_task(ictx, &progress_context);
  boost::thread thread(boost::ref(flatten_task));

  ASSERT_TRUE(wait_for_notifies(*ictx));

  NotifyOps expected_notify_ops;
  expected_notify_ops += NOTIFY_OP_FLATTEN;
  ASSERT_EQ(expected_notify_ops, m_notifies);

  AsyncRequestId async_request_id;
  ASSERT_TRUE(extract_async_request_id(NOTIFY_OP_FLATTEN, &async_request_id));

  ASSERT_EQ(0, notify_async_complete(ictx, async_request_id, -ESHUTDOWN));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(-ESHUTDOWN, flatten_task.result);
}

TEST_F(TestImageWatcher, NotifyAsyncRequestTimedOut) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  ictx->request_timed_out_seconds = 0;

  ASSERT_EQ(0, register_image_watch(*ictx));
  ASSERT_EQ(0, lock_image(*ictx, LOCK_EXCLUSIVE,
			  "auto " + stringify(m_watch_ctx->get_handle())));

  m_notify_acks = {{NOTIFY_OP_FLATTEN, create_response_message(0)}};

  ProgressContext progress_context;
  FlattenTask flatten_task(ictx, &progress_context);
  boost::thread thread(boost::ref(flatten_task));

  ASSERT_TRUE(wait_for_notifies(*ictx));

  ASSERT_TRUE(thread.timed_join(boost::posix_time::seconds(10)));
  ASSERT_EQ(-ERESTART, flatten_task.result);
}

TEST_F(TestImageWatcher, PeerRequestsLock) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));
  ASSERT_EQ(0, register_image_watch(*ictx));

  register_lock_listener(*ictx);
  m_notify_acks = {{NOTIFY_OP_ACQUIRED_LOCK, {}}};

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ictx->image_watcher->request_lock();
  }

  ASSERT_TRUE(wait_for_notifies(*ictx));

  {
    RWLock::RLocker owner_locker(ictx->owner_lock);
    ASSERT_TRUE(ictx->image_watcher->is_lock_owner());
  }

  // if journaling is enabled, ensure we wait for it to replay since
  // it will block our peer request
  std::string buffer(256, '1');
  ictx->aio_work_queue->write(0, buffer.size(), buffer.c_str(), 0);

  {
    Mutex::Locker l(m_callback_lock);
    m_notifies.clear();
    m_notify_acks = {{NOTIFY_OP_RELEASED_LOCK, {}}};
  }

  bufferlist bl;
  {
    ENCODE_START(1, 1, bl);
    ::encode(NOTIFY_OP_REQUEST_LOCK, bl);
    ENCODE_FINISH(bl);
  }
  ASSERT_EQ(0, m_ioctx.notify2(ictx->header_oid, bl, 5000, NULL));

  ASSERT_TRUE(wait_for_releasing_lock(*ictx));
  ASSERT_TRUE(wait_for_notifies(*ictx));
}
