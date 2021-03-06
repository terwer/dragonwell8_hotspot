/*
 * Copyright (c) 2012, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jfr/recorder/jfrRecorder.hpp"
#include "jfr/periodic/sampling/jfrCallTrace.hpp"
#include "jfr/periodic/sampling/jfrThreadSampler.hpp"
#include "jfr/recorder/stacktrace/jfrStackTraceRepository.hpp"
#include "jfr/recorder/access/jfrOptionSet.hpp"
#include "jfr/utilities/jfrTraceTime.hpp"
#include "jfr/utilities/jfrLog.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.inline.hpp"
#include "trace/tracing.hpp"
#include "tracefiles/traceEventIds.hpp"

static bool in_java_sample(JavaThread* thread) {
  switch (thread->thread_state()) {
  case _thread_new:
  case _thread_uninitialized:
  case _thread_new_trans:
  case _thread_in_vm_trans:
  case _thread_blocked_trans:
  case _thread_in_native_trans:
  case _thread_blocked:
  case _thread_in_vm:
  case _thread_in_native:
  case _thread_in_Java_trans:
    break;
  case _thread_in_Java:
    return true;
  default:
    ShouldNotReachHere();
    break;
  }
  return false;
}

static bool in_native_sample(JavaThread* thread) {
  switch (thread->thread_state()) {
  case _thread_new:
  case _thread_uninitialized:
  case _thread_new_trans:
  case _thread_blocked_trans:
  case _thread_blocked:
  case _thread_in_vm:
  case _thread_in_vm_trans:
  case _thread_in_Java_trans:
  case _thread_in_Java:
  case _thread_in_native_trans:
    break;
  case _thread_in_native:
    return true;
  default:
    ShouldNotReachHere();
    break;
  }
  return false;
}

class JfrThreadSampleClosure {
 public:
  JfrThreadSampleClosure(EventExecutionSample* events, EventNativeMethodSample* events_native);
  ~JfrThreadSampleClosure() {}
  EventExecutionSample* next_event() { return &_events[_added_java++]; }
  EventNativeMethodSample* next_event_native() { return &_events_native[_added_native++]; }
  void commit_events();
  int added() const { return _added_java; }
  JfrSampleType do_sample_thread(JavaThread* thread, JfrStackFrame* frames, u4 max_frames, bool java_sample, bool native_sample);
  int java_entries() { return _added_java; }
  int native_entries() { return _added_native; }

 private:
  bool sample_thread_in_java(JavaThread* thread, JfrStackFrame* frames, u4 max_frames);
  bool sample_thread_in_native(JavaThread* thread, JfrStackFrame* frames, u4 max_frames);
  EventExecutionSample* _events;
  EventNativeMethodSample* _events_native;
  Thread* _self;
  int _added_java;
  int _added_native;
};

class OSThreadSampler : public os::SuspendedThreadTask {
 public:
  OSThreadSampler(JavaThread* thread,
                  JfrThreadSampleClosure& closure,
                  JfrStackFrame *frames,
                  u4 max_frames) : os::SuspendedThreadTask((Thread*)thread),
    _success(false),
    _stacktrace(frames, max_frames),
    _closure(closure),
    _suspend_time(0) {}

  void take_sample();
  void do_task(const os::SuspendedThreadTaskContext& context);
  void protected_task(const os::SuspendedThreadTaskContext& context);
  bool success() const { return _success; }
  const JfrStackTrace& stacktrace() const { return _stacktrace; }

 private:
  bool _success;
  JfrStackTrace _stacktrace;
  JfrThreadSampleClosure& _closure;
  JfrTraceTime _suspend_time;
};

class OSThreadSamplerCallback : public os::CrashProtectionCallback {
 public:
  OSThreadSamplerCallback(OSThreadSampler& sampler, const os::SuspendedThreadTaskContext &context) :
    _sampler(sampler), _context(context) {
  }
  virtual void call() {
    _sampler.protected_task(_context);
  }
 private:
  OSThreadSampler& _sampler;
  const os::SuspendedThreadTaskContext& _context;
};

void OSThreadSampler::do_task(const os::SuspendedThreadTaskContext& context) {
#ifndef ASSERT
  guarantee(JfrOptionSet::sample_protection(), "Sample Protection should be on in product builds");
#endif
  assert(0 == _suspend_time, "already timestamped!");
  _suspend_time = JfrTraceTime::now();

  if (JfrOptionSet::sample_protection()) {
    OSThreadSamplerCallback cb(*this, context);
    os::ThreadCrashProtection crash_protection;
    if (!crash_protection.call(cb)) {
      log_error(jfr)("Thread method sampler crashed");
    }
  } else {
    protected_task(context);
  }
}

/*
* From this method and down the call tree we attempt to protect against crashes
* using a signal handler / __try block. Don't take locks, rely on destructors or
* leave memory (in case of signal / exception) in an inconsistent state. */
void OSThreadSampler::protected_task(const os::SuspendedThreadTaskContext& context) {
  JavaThread* jth = (JavaThread*)context.thread();
  // Skip sample if we signaled a thread that moved to other state
  if (!in_java_sample(jth)) {
    return;
  }
  JfrGetCallTrace trace(true, jth);
  frame topframe;
  if (trace.get_topframe(context.ucontext(), topframe)) {
    if (_stacktrace.record_thread(*jth, topframe)) {
      /* If we managed to get a topframe and a stacktrace, create an event
      * and put it into our array. We can't call Jfr::_stacktraces.add()
      * here since it would allocate memory using malloc. Doing so while
      * the stopped thread is inside malloc would deadlock. */
      _success = true;
      EventExecutionSample *ev = _closure.next_event();
      ev->set_starttime(_suspend_time);
      ev->set_endtime(JfrTraceTime(1)); // fake to not take an end time
      ev->set_sampledThread(THREAD_TRACE_ID(jth));
      ev->set_state(java_lang_Thread::get_thread_status(jth->threadObj()));
    }
  }
}

void OSThreadSampler::take_sample() {
  run();
}

class JfrNativeSamplerCallback : public os::CrashProtectionCallback {
 public:
  JfrNativeSamplerCallback(JfrThreadSampleClosure& closure, JavaThread* jt, JfrStackFrame* frames, u4 max_frames) :
    _closure(closure), _jt(jt), _stacktrace(frames, max_frames), _success(false) {
  }
  virtual void call();
  bool success() { return _success; }
  JfrStackTrace& stacktrace() { return _stacktrace; }

 private:
  JfrThreadSampleClosure& _closure;
  JavaThread* _jt;
  JfrStackTrace _stacktrace;
  bool _success;
};

static void write_native_event(JfrThreadSampleClosure& closure, JavaThread* jt) {
  EventNativeMethodSample *ev = closure.next_event_native();
  ev->set_starttime(JfrTraceTime(0));
  ev->set_sampledThread(THREAD_TRACE_ID(jt));
  ev->set_state(java_lang_Thread::get_thread_status(jt->threadObj()));
}

void JfrNativeSamplerCallback::call() {
  // When a thread is only attach it will be native without a last java frame
  if (!_jt->has_last_Java_frame()) {
    return;
  }

  frame topframe = _jt->last_frame();
  frame first_java_frame;
  Method* method = NULL;
  JfrGetCallTrace gct(false, _jt);
  if (!gct.find_top_frame(topframe, &method, first_java_frame)) {
    return;
  }
  if (method == NULL) {
    return;
  }
  topframe = first_java_frame;
  _success = _stacktrace.record_thread(*_jt, topframe);
  if (_success) {
    write_native_event(_closure, _jt);
  }
}

bool JfrThreadSampleClosure::sample_thread_in_java(JavaThread* thread, JfrStackFrame* frames, u4 max_frames) {
  OSThreadSampler sampler(thread, *this, frames, max_frames);
  sampler.take_sample();
  /* We don't want to allocate any memory using malloc/etc while the thread
  * is stopped, so everything is stored in stack allocated memory until this
  * point where the thread has been resumed again, if the sampling was a success
  * we need to store the stacktrace in the stacktrace repository and update
  * the event with the id that was returned. */
  if (!sampler.success()) {
    return false;
  }
  EventExecutionSample *event = &_events[_added_java - 1];
  traceid id = JfrStackTraceRepository::add(sampler.stacktrace());
  assert(id != 0, "Stacktrace id should not be 0");
  event->set_stackTrace(id);
  return true;
}

bool JfrThreadSampleClosure::sample_thread_in_native(JavaThread* thread, JfrStackFrame* frames, u4 max_frames) {
  JfrNativeSamplerCallback cb(*this, thread, frames, max_frames);
  if (JfrOptionSet::sample_protection()) {
    os::ThreadCrashProtection crash_protection;
    if (!crash_protection.call(cb)) {
      log_error(jfr)("Thread method sampler crashed for native");
    }
  } else {
    cb.call();
  }
  if (!cb.success()) {
    return false;
  }
  EventNativeMethodSample *event = &_events_native[_added_native - 1];
  traceid id = JfrStackTraceRepository::add(cb.stacktrace());
  assert(id != 0, "Stacktrace id should not be 0");
  event->set_stackTrace(id);
  return true;
}

void JfrThreadSampleClosure::commit_events() {
  for (int i = 0; i < _added_java; ++i) {
    _events[i].commit();
  }
  for (int i = 0; i < _added_native; ++i) {
    _events_native[i].commit();
  }
}

JfrThreadSampleClosure::JfrThreadSampleClosure(EventExecutionSample* events, EventNativeMethodSample* events_native) :
  _events(events),
  _events_native(events_native),
  _self(Thread::current()),
  _added_java(0),
  _added_native(0) {
}

static void clear_transition_block(JavaThread* jt) {
  jt->clear_trace_flag();
  JfrThreadData* jtd = jt->trace_data();
  if (jtd->is_trace_block()) {
    MutexLockerEx ml(JfrThreadSampler::transition_block(), Mutex::_no_safepoint_check_flag);
    JfrThreadSampler::transition_block()->notify_all();
  }
}

JfrSampleType JfrThreadSampleClosure::do_sample_thread(JavaThread* thread, JfrStackFrame* frames, u4 max_frames, bool java_sample, bool native_sample) {
  assert(Threads_lock->owned_by_self(), "Holding the thread table lock.");
  if (thread->is_hidden_from_external_view()) {
    return NO_SAMPLE;
  }
  if (thread->in_deopt_handler()) {
    return NO_SAMPLE;
  }
  JfrSampleType ret = NO_SAMPLE;
  thread->set_trace_flag();
  if (!UseMembar) {
    os::serialize_thread_states();
  }
  if (in_java_sample(thread) && java_sample) {
    ret = sample_thread_in_java(thread, frames, max_frames) ? JAVA_SAMPLE : NO_SAMPLE;
  } else if (in_native_sample(thread) && native_sample) {
    ret = sample_thread_in_native(thread, frames, max_frames) ? NATIVE_SAMPLE : NO_SAMPLE;
  }
  clear_transition_block(thread);
  return ret;
}

Monitor* JfrThreadSampler::_transition_block_lock = new Monitor(Mutex::leaf, "Trace block", true);

JfrThreadSampler::JfrThreadSampler(size_t interval_java, size_t interval_native, u4 max_frames) :
  _frames(JfrCHeapObj::new_array<JfrStackFrame>(max_frames)),
  _last_thread_java(NULL),
  _last_thread_native(NULL),
  _interval_java(interval_java),
  _interval_native(interval_native),
  _cur_index(-1),
  _max_frames(max_frames),
  _should_terminate(false) {
}

JfrThreadSampler::~JfrThreadSampler() {
  JfrCHeapObj::free(_frames, sizeof(JfrStackFrame) * _max_frames);
}

void JfrThreadSampler::on_javathread_suspend(JavaThread* thread) {
  JfrThreadData* jtd = thread->trace_data();
  jtd->set_trace_block();
  {
    MutexLockerEx ml(transition_block(), Mutex::_no_safepoint_check_flag);
    while (thread->is_trace_suspend()) {
      transition_block()->wait(true);
    }
    jtd->clear_trace_block();
  }
}

int JfrThreadSampler::find_index_of_JavaThread(JavaThread** t_list, uint length, JavaThread *target) {
  assert(Threads_lock->owned_by_self(), "Holding the thread table lock.");
  if (target == NULL) {
    return -1;
  }
  for (uint i = 0; i < length; i++) {
    if (target == t_list[i]) {
      return (int)i;
    }
  }
  return -1;
}

JavaThread* JfrThreadSampler::next_thread(JavaThread** t_list, uint length, JavaThread* first_sampled, JavaThread* current) {
  assert(Threads_lock->owned_by_self(), "Holding the thread table lock.");
  if (current == NULL) {
    _cur_index = 0;
    return t_list[_cur_index];
  }

  if (_cur_index == -1 || t_list[_cur_index] != current) {
    // 'current' is not at '_cur_index' so find it:
    _cur_index = find_index_of_JavaThread(t_list, length, current);
    assert(_cur_index != -1, "current JavaThread should be findable.");
  }
  _cur_index++;

  JavaThread* next = NULL;
  // wrap
  if ((uint)_cur_index >= length) {
    _cur_index = 0;
  }
  next = t_list[_cur_index];

  // sample wrap
  if (next == first_sampled) {
    return NULL;
  }
  return next;
}

void JfrThreadSampler::enroll() {
  if (os::create_thread(this, os::os_thread)) {
    os::start_thread(this);
  } else {
    log_error(jfr)("Failed to create thread for thread sampling");
  }
}

void JfrThreadSampler::disenroll() {
  _should_terminate = true;
}

static jlong get_monotonic_ms() {
  return os::javaTimeNanos() / 1000000;
}

void JfrThreadSampler::run() {
  jlong last_java_ms = get_monotonic_ms();
  jlong last_native_ms = last_java_ms;
  while (!_should_terminate) {
    jlong java_interval = _interval_java == 0 ? max_jlong : MAX2<jlong>(_interval_java, 10);
    jlong native_interval = _interval_native == 0 ? max_jlong : MAX2<jlong>(_interval_native, 10);

    jlong now_ms = get_monotonic_ms();

    jlong next_j = java_interval + last_java_ms - now_ms;
    jlong next_n = native_interval + last_native_ms - now_ms;

    jlong sleep_to_next = MIN2<jlong>(next_j, next_n);

    if (sleep_to_next > 0) {
      os::naked_short_sleep(sleep_to_next);
    }

    if ((next_j - sleep_to_next) <= 0) {
      task_stacktrace(JAVA_SAMPLE, &_last_thread_java);
      last_java_ms = get_monotonic_ms();
    }
    if ((next_n - sleep_to_next) <= 0) {
      task_stacktrace(NATIVE_SAMPLE, &_last_thread_native);
      last_native_ms = get_monotonic_ms();
    }
  }
  delete this;
}

static const int MAX_NR_OF_SAMPLES = 5;

void JfrThreadSampler::task_stacktrace(JfrSampleType type, JavaThread** last_thread) {
  ResourceMark rm;
  EventExecutionSample samples[MAX_NR_OF_SAMPLES];
  EventNativeMethodSample samples_native[MAX_NR_OF_SAMPLES];
  JfrThreadSampleClosure sample_task(samples, samples_native);

  int num_samples = 0;
  {
    elapsedTimer sample_time;
    sample_time.start();

    {
      MonitorLockerEx tlock(Threads_lock, Mutex::_allow_vm_block_flag);
      int max_threads = Threads::number_of_threads();
      assert(max_threads >= 0, "Threads list is empty");
      uint index = 0;
      JavaThread** threads_list = NEW_C_HEAP_ARRAY(JavaThread *, max_threads, mtInternal);
      for (JavaThread* tp = Threads::first(); tp != NULL; tp = tp->next()) {
        threads_list[index++] = tp;
      }
      JavaThread* current = Threads::includes(*last_thread) ? *last_thread : NULL;
      JavaThread* start = NULL;

      while (num_samples < MAX_NR_OF_SAMPLES) {
        current = next_thread(threads_list, index, start, current);
        if (current == NULL) {
          break;
        }
        if (start == NULL) {
          start = current;  // remember thread where we started sampling
        }
        if (current->is_Compiler_thread()) {
          continue;
        }
        *last_thread = current;  // remember thread we last sampled
        JfrSampleType ret = sample_task.do_sample_thread(current, _frames, _max_frames, type == JAVA_SAMPLE, type == NATIVE_SAMPLE);
        switch (type) {
        case JAVA_SAMPLE:
        case NATIVE_SAMPLE:
          ++num_samples;
          break;
        default:
          break;
        }
      }
      FREE_C_HEAP_ARRAY(JavaThread *, threads_list, mtInternal);
      // release Threads_lock
    }
    sample_time.stop();
    log_trace(jfr)("JFR thread sampling done in %3.7f secs with %d java %d native samples",
      sample_time.seconds(), sample_task.java_entries(), sample_task.native_entries());
  }
  if (num_samples>0) {
    sample_task.commit_events();
  }
}

static JfrThreadSampling* _instance = NULL;

JfrThreadSampling& JfrThreadSampling::instance() {
  return *_instance;
}

JfrThreadSampling* JfrThreadSampling::create() {
  assert(_instance == NULL, "invariant");
  _instance = new JfrThreadSampling();
  return _instance;
}

void JfrThreadSampling::destroy() {
  if (_instance != NULL) {
    delete _instance;
    _instance = NULL;
  }
}

JfrThreadSampling::JfrThreadSampling() : _sampler(NULL) {}

JfrThreadSampling::~JfrThreadSampling() {
  log_info(jfr)("Disrolling thread sampler");
  stop_sampler();
}

static void log(size_t interval_java, size_t interval_native) {
  log_info(jfr)("Updated thread sampler for java: " SIZE_FORMAT"  ms, native " SIZE_FORMAT " ms", interval_java, interval_native);
}

void JfrThreadSampling::start_sampler(size_t interval_java, size_t interval_native) {
  assert(_sampler == NULL, "invariant");
  log_info(jfr)("Enrolling thread sampler");
  _sampler = new JfrThreadSampler(interval_java, interval_native, JfrOptionSet::stackdepth());
  _sampler->enroll();
}

void JfrThreadSampling::stop_sampler() {
  if (_sampler != NULL) {
    _sampler->disenroll();
    _sampler = NULL;
  }
}

void JfrThreadSampling::set_sampling_interval(bool java_interval, size_t period) {
  size_t interval_java = 0;
  size_t interval_native = 0;
  if (_sampler != NULL) {
    interval_java = _sampler->get_java_interval();
    interval_native = _sampler->get_native_interval();
  }

  if (java_interval) {
    interval_java = period;
  } else {
    interval_native = period;
  }

  if (interval_java > 0 || interval_native > 0) {
    if (_sampler == NULL) {
      start_sampler(interval_java, interval_native);
    } else {
      _sampler->set_java_interval(interval_java);
      _sampler->set_native_interval(interval_native);
    }
    assert(_sampler != NULL, "invariant");
    log(interval_java, interval_native);
  } else if (_sampler != NULL) {
    log_info(jfr)("Disrolling thread sampler");
    stop_sampler();
  }
}

void JfrThreadSampling::set_java_sample_interval(size_t period) {
  if (_instance == NULL && 0 == period) {
    return;
  }
  instance().set_sampling_interval(true, period);
}

void JfrThreadSampling::set_native_sample_interval(size_t period) {
  if (_instance == NULL && 0 == period) {
    return;
  }
  instance().set_sampling_interval(false, period);
}

