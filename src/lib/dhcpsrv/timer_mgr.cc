// Copyright (C) 2015 Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <asio.hpp>
#include <asiolink/io_service.h>
#include <dhcp/iface_mgr.h>
#include <dhcpsrv/dhcpsrv_log.h>
#include <dhcpsrv/timer_mgr.h>
#include <exceptions/exceptions.h>
#include <boost/bind.hpp>
#include <utility>

using namespace isc;
using namespace isc::asiolink;
using namespace isc::util;
using namespace isc::util::thread;

namespace isc {
namespace dhcp {

TimerMgr&
TimerMgr::instance() {
    static TimerMgr timer_mgr;
    return (timer_mgr);
}

TimerMgr::TimerMgr()
    : io_service_(new IOService()), thread_(),
      registered_timers_() {
}

TimerMgr::~TimerMgr() {
    // Stop the thread, but do not unregister any timers. Unregistering
    // the timers could cause static deinitialization fiasco between the
    // TimerMgr and IfaceMgr. By now, the caller should have unregistered
    // the timers.
    stopThread();
}

void
TimerMgr::registerTimer(const std::string& timer_name,
                        const IntervalTimer::Callback& callback,
                        const long interval,
                        const IntervalTimer::Mode& scheduling_mode) {

    LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
              DHCPSRV_TIMERMGR_REGISTER_TIMER)
        .arg(timer_name)
        .arg(interval);

    // Timer name must not be empty.
    if (timer_name.empty()) {
        isc_throw(BadValue, "registered timer name must not be empty");
    }

    // Must not register two timers under the same name.
    if (registered_timers_.find(timer_name) != registered_timers_.end()) {
        isc_throw(BadValue, "trying to register duplicate timer '"
                  << timer_name << "'");
    }

    // Must not register new timer when the worker thread is running. Note
    // that worker thread is using IO service and trying to register a new
    // timer while IO service is being used would result in hang.
    if (thread_) {
        isc_throw(InvalidOperation, "unable to register new timer when the"
                  " timer manager's worker thread is running");
    }

    // Create a structure holding the configuration for the timer. It will
    // create the instance if the IntervalTimer and WatchSocket. It will
    // also hold the callback, interval and scheduling mode parameters.
    // This may throw a WatchSocketError if the socket creation fails.
    TimerInfoPtr timer_info(new TimerInfo(getIOService(), callback,
                                          interval, scheduling_mode));

    // Register the WatchSocket in the IfaceMgr and register our own callback
    // to be executed when the data is received over this socket. The only time
    // this may fail is when the socket failed to open which would have caused
    // an exception in the previous call. So we should be safe here.
    IfaceMgr::instance().addExternalSocket(timer_info->watch_socket_.getSelectFd(),
                                           boost::bind(&TimerMgr::ifaceMgrCallback,
                                                       this, timer_name));

    // Actually register the timer.
    registered_timers_.insert(std::pair<std::string, TimerInfoPtr>(timer_name,
                                                                   timer_info));
}

void
TimerMgr::unregisterTimer(const std::string& timer_name) {

    LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
              DHCPSRV_TIMERMGR_UNREGISTER_TIMER)
        .arg(timer_name);

    if (thread_) {
        isc_throw(InvalidOperation, "unable to unregister timer "
                  << timer_name << " while worker thread is running");
    }

    // Find the timer with specified name.
    TimerInfoMap::iterator timer_info_it = registered_timers_.find(timer_name);

    // Check if the timer has been registered.
    if (timer_info_it == registered_timers_.end()) {
        isc_throw(BadValue, "unable to unregister non existing timer '"
                  << timer_name << "'");
    }

    // Cancel any pending asynchronous operation and stop the timer.
    cancel(timer_name);

    const TimerInfoPtr& timer_info = timer_info_it->second;

    // Unregister the watch socket from the IfaceMgr.
    IfaceMgr::instance().deleteExternalSocket(timer_info->watch_socket_.getSelectFd());

    // Remove the timer.
    registered_timers_.erase(timer_info_it);
}

void
TimerMgr::unregisterTimers() {

    LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
              DHCPSRV_TIMERMGR_UNREGISTER_ALL_TIMERS);

    // Copy the map holding timers configuration. This is required so as
    // we don't cut the branch which we're sitting on when we will be
    // erasing the timers. We're going to iterate over the register timers
    // and remove them with the call to unregisterTimer function. But this
    // function will remove them from the register_timers_ map. If we
    // didn't work on the copy here, our iterator would invalidate. The
    // TimerInfo structure is copyable and since it is using the shared
    // pointers the copy is not expensive. Also this function is called when
    // the process terminates so it is not critical for performance.
    TimerInfoMap registered_timers_copy(registered_timers_);

    // Iterate over the existing timers and unregister them.
    for (TimerInfoMap::iterator timer_info_it = registered_timers_copy.begin();
         timer_info_it != registered_timers_copy.end(); ++timer_info_it) {
        unregisterTimer(timer_info_it->first);
    }
}

void
TimerMgr::setup(const std::string& timer_name) {

    LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
              DHCPSRV_TIMERMGR_START_TIMER)
        .arg(timer_name);

   // Check if the specified timer exists.
   TimerInfoMap::const_iterator timer_info_it = registered_timers_.find(timer_name);
   if (timer_info_it == registered_timers_.end()) {
       isc_throw(BadValue, "unable to setup timer '" << timer_name << "': "
                 "no such timer registered");
   }

   // Schedule the execution of the timer using the parameters supplied
   // during the registration.
   const TimerInfoPtr& timer_info = timer_info_it->second;
   timer_info->interval_timer_.setup(boost::bind(&TimerMgr::timerCallback, this, timer_name),
                                     timer_info->interval_,
                                     timer_info->scheduling_mode_);
}

void
TimerMgr::cancel(const std::string& timer_name) {

    LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
              DHCPSRV_TIMERMGR_STOP_TIMER)
        .arg(timer_name);

    // Find the timer of our interest.
    TimerInfoMap::const_iterator timer_info_it = registered_timers_.find(timer_name);
    if (timer_info_it == registered_timers_.end()) {
        isc_throw(BadValue, "unable to cancel timer '" << timer_name << "': "
                  "no such timer registered");
    }
    // Cancel the timer.
    timer_info_it->second->interval_timer_.cancel();
    // Clear watch socket, if ready.
    timer_info_it->second->watch_socket_.clearReady();
}

void
TimerMgr::startThread() {
    // Do not start the thread if the thread is already there.
    if (!thread_) {
        // Only log it if we really start the thread.
        LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
                  DHCPSRV_TIMERMGR_START_THREAD);

        // The thread will simply run IOService::run(), which is a blocking call
        // to keep running handlers for all timers according to how they have
        // been scheduled.
        thread_.reset(new Thread(boost::bind(&IOService::run, &getIOService())));
    }
}

void
TimerMgr::stopThread(const bool run_pending_callbacks) {
    // If thread is not running, this is no-op.
    if (thread_) {
        // Only log it if we really have something to stop.
        LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE,
                  DHCPSRV_TIMERMGR_STOP_THREAD);

        // Stop the IO Service. This will break the IOService::run executed in the
        // worker thread. The thread will now terminate.
        getIOService().stop();
        // Some of the watch sockets may be already marked as ready and
        // have some pending callbacks to be executed. If the caller
        // wants us to run the callbacks we clear the sockets and run
        // them. If pending callbacks shouldn't be executed, this will
        // only clear the sockets (which should be substantially faster).
        clearReadySockets(run_pending_callbacks);
        // Wait for the thread to terminate.
        thread_->wait();
        // Set the thread pointer to NULL to indicate that the thread is not running.
        thread_.reset();
        // IO Service has to be reset so as we can call run() on it again if we
        // desire (using the startThread()).
        getIOService().get_io_service().reset();
    }
}
IOService&
TimerMgr::getIOService() const {
    return (*io_service_);
}

void
TimerMgr::timerCallback(const std::string& timer_name) {
    // Find the specified timer setup.
    TimerInfoMap::iterator timer_info_it = registered_timers_.find(timer_name);
    if (timer_info_it != registered_timers_.end()) {
        // We will mark watch socket ready - write data to a socket to
        // interrupt the execution of the select() function. This is
        // executed from the worker thread.
        const TimerInfoPtr& timer_info = timer_info_it->second;
        timer_info->watch_socket_.markReady();
    }
}

void
TimerMgr::ifaceMgrCallback(const std::string& timer_name) {
    // Find the specified timer setup.
    TimerInfoMap::iterator timer_info_it = registered_timers_.find(timer_name);
    if (timer_info_it != registered_timers_.end()) {
        // We're executing a callback function from the Interface Manager.
        // This callback function is executed when the call to select() is
        // interrupted as a result of receiving some data over the watch
        // socket. We have to clear the socket which has been marked as
        // ready. Then execute the callback function supplied by the
        // TimerMgr user to perform custom actions on the expiration of
        // the given timer.
        handleReadySocket(timer_info_it, true);
    }
}

void
TimerMgr::clearReadySockets(const bool run_pending_callbacks) {
    for (TimerInfoMap::iterator timer_info_it = registered_timers_.begin();
         timer_info_it != registered_timers_.end(); ++timer_info_it) {
        handleReadySocket(timer_info_it, run_pending_callbacks);
   }
}

template<typename Iterator>
void
TimerMgr::handleReadySocket(Iterator timer_info_iterator,
                            const bool run_callback) {
    timer_info_iterator->second->watch_socket_.clearReady();

    if (run_callback) {
        // Running user-defined operation for the timer. Logging it
        // on the slightly lower debug level as there may be many
        // such traces.
        LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_TRACE_DETAIL,
                  DHCPSRV_TIMERMGR_RUN_TIMER_OPERATION)
            .arg(timer_info_iterator->first);
        timer_info_iterator->second->user_callback_();
    }
}

} // end of namespace isc::dhcp
} // end of namespace isc