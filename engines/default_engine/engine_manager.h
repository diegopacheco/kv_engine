/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

/**
    Engine manager provides a C API for the managment of default_engine 'handles'.

    Creation/Deletion and the item scrubber thread are all managed by this module.
**/

#ifdef __cplusplus

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "scrubber_task.h"

/**
    Create/Delete of engines from one location.
    Manages the scrubber task and handles global shutdown
**/
class EngineManager {
public:

    EngineManager();
    ~EngineManager();

    struct default_engine* createEngine();

    /**
        Delete engine struct
    **/
    void deleteEngine(struct default_engine* engine);

    /**
        Request that the scrubber destroy's this engine.
        Scrubber will delete the object.
    **/
    void requestDestroyEngine(struct default_engine* engine);

    /**
        Request that the engine is scrubbed.
    **/
    void scrubEngine(struct default_engine* engine);

    /**
        Set the shutdown flag so that we can clean up
        1) no new engine's can be created.
        2) the scrubber can be notified to exit and joined.
    **/
    void shutdown();

private:
    ScrubberTask scrubberTask;
    std::atomic<bool> shuttingdown;
    std::mutex lock;
    std::unordered_set<struct default_engine*> engines;

};

extern "C" {
#endif

/*
 * Create a new engine instance.
 * Returns NULL for failure.
 */
struct default_engine* engine_manager_create_engine();

/*
 * Delete the engine instance.
 * Deletion is performed by a background thread.
 * On return from this method the caller must not use the pointer as
 * it will be deleted at any time.
 */
void engine_manager_delete_engine(struct default_engine* engine);

/*
 * Request that a scrub of the engine is performed.
 * Scrub is perfromed by a background thread.
 */
void engine_manager_scrub_engine(struct default_engine* engine);

/*
 * Perform global shutdown in prepration for unloading of the shared object.
 * This method will block until background threads are joined.
 */
void engine_manager_shutdown();

#ifdef __cplusplus
}
#endif
