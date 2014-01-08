/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef SRC_SCHEDULER_H_
#define SRC_SCHEDULER_H_ 1

#include "config.h"

#include <deque>
#include <list>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "atomic.h"
#include "common.h"
#include "mutex.h"
#include "objectregistry.h"
#include "ringbuffer.h"
#include "tasks.h"
#include "task_type.h"

#define TASK_LOG_SIZE 20
#define MIN_SLEEP_TIME 2.0

class ExecutorPool;
class ExecutorThread;
class WorkLoadPolicy;

typedef enum {
    EXECUTOR_CREATING,
    EXECUTOR_RUNNING,
    EXECUTOR_WAITING,
    EXECUTOR_SLEEPING,
    EXECUTOR_SHUTDOWN,
    EXECUTOR_DEAD
} executor_state_t;


/**
 * Log entry for previous job runs.
 */
class TaskLogEntry {
public:

    // This is useful for the ringbuffer to initialize
    TaskLogEntry() : name("invalid"), duration(0) {}
    TaskLogEntry(const std::string &n, const hrtime_t d, rel_time_t t = 0)
        : name(n), ts(t), duration(d) {}

    /**
     * Get the name of the job.
     */
    std::string getName() const { return name; }

    /**
     * Get the amount of time (in microseconds) this job ran.
     */
    hrtime_t getDuration() const { return duration; }

    /**
     * Get a timestamp indicating when this thing started.
     */
    rel_time_t getTimestamp() const { return ts; }

private:
    std::string name;
    rel_time_t ts;
    hrtime_t duration;
};

class ExecutorThread {
    friend class ExecutorPool;
public:

    ExecutorThread(ExecutorPool *m, size_t startingQueue, const std::string nm)
        : manager(m), startIndex(startingQueue), name(nm),
          state(EXECUTOR_CREATING), taskStart(0),
          tasklog(TASK_LOG_SIZE), slowjobs(TASK_LOG_SIZE), currentTask(NULL),
          curTaskType(-1) { set_max_tv(waketime); }

    ~ExecutorThread() {
        LOG(EXTENSION_LOG_INFO, "Executor killing %s", name.c_str());
    }

    void start(void);

    void run(void);

    void stop(bool wait=true);

    void shutdown() { state = EXECUTOR_SHUTDOWN; }

    void schedule(ExTask &task);

    void reschedule(ExTask &task);

    void wake(ExTask &task);

    const std::string& getName() const { return name; }

    const std::string getTaskName() const {
        if (currentTask) {
            return currentTask->getDescription();
        } else {
            return std::string("Not currently running any task");
        }
    }

    hrtime_t getTaskStart() const { return taskStart; }

    const std::string getStateName();

    const std::vector<TaskLogEntry> getLog() { return tasklog.contents(); }

    const std::vector<TaskLogEntry> getSlowLog() { return slowjobs.contents();}

private:

    cb_thread_t thread;
    ExecutorPool *manager;
    size_t startIndex;
    const std::string name;
    executor_state_t state;

    struct timeval waketime; // set to the earliest

    hrtime_t taskStart;
    RingBuffer<TaskLogEntry> tasklog;
    RingBuffer<TaskLogEntry> slowjobs;

    ExTask currentTask;
    int curTaskType;
};

#endif  // SRC_SCHEDULER_H_
