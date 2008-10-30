/*
 * opencog/server/CogServer.cc
 *
 * Copyright (C) 2002-2007 Novamente LLC
 * Copyright (C) 2008 by Singularity Institute for Artificial Intelligence
 * All Rights Reserved
 *
 * Written by Andre Senna <senna@vettalabs.com>
 *            Gustavo Gama <gama@vettalabs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "CogServer.h"

#include <tr1/memory>
#include <tr1/functional>

#include <time.h>
#ifdef WIN32
#include <winsock2.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <sys/time.h>
#endif

#include <opencog/atomspace/AtomSpace.h>
#include <opencog/server/Agent.h>
#include <opencog/server/ConsoleSocket.h>
#include <opencog/server/NetworkServer.h>
#include <opencog/server/Request.h>
#include <opencog/util/Config.h>
#include <opencog/util/Logger.h>
#include <opencog/util/exceptions.h>
#include <opencog/util/misc.h>
#include "load-file.h"

using namespace opencog;

namespace opencog {
struct equal_to_id : public std::binary_function<const Agent*, const std::string&, bool>
{
    bool operator()(const Agent* a, const std::string& cid) const {
        return (a->classinfo().id != cid);
    }
};
}

CogServer* CogServer::createInstance() {
    return new CogServer();
}

CogServer::~CogServer()
{
    disableNetworkServer();

    // unload all modules
    for (ModuleMap::iterator it = modules.begin(); it != modules.end(); ++it) {
        // retest the key because it might have been removed already
        if (modules.find(it->first) != modules.end()) {
            logger().debug("[CogServer] removing module %s\"", it->first.c_str());
            ModuleData mdata = it->second;

            // cache filename and id to erase the entries from the modules map
            std::string filename = mdata.filename;
            std::string id = mdata.id;

            // invoke the module's unload function
            (*mdata.unloadFunction)(mdata.module);

            // erase the map entries (one with the filename as key, and one with the module)
            // id as key
            modules.erase(filename);
            modules.erase(id);
        }
    }
}

CogServer::CogServer() : cycleCount(1)
{
    if (atomSpace != NULL) delete atomSpace;
    atomSpace = new AtomSpace();

    pthread_mutex_init(&messageQueueLock, NULL);
}

NetworkServer& CogServer::networkServer()
{
    return _networkServer;
}

void CogServer::enableNetworkServer()
{
    _networkServer.start();
    _networkServer.addListener<ConsoleSocket>(config().get_int("SERVER_PORT"));

}

void CogServer::disableNetworkServer()
{
    _networkServer.stop();
}

void CogServer::serverLoop()
{
    struct timeval timer_start, timer_end;
    time_t elapsed_time;
    time_t cycle_duration = config().get_int("SERVER_CYCLE_DURATION") * 1000;

    // XXX I've hard-coded a file path here, it assumes that the
    // cog server is running from the "bin" directory, (where "make"
    // was typed). Iwithout a more general file-path mangling mechanism,
    // I am not sure what else to do. But this should be fixed XXX.
    load_scm_file("../src/scm/type_constructors.scm");

    logger().info("opencog server ready.");

    gettimeofday(&timer_start, NULL);
    for (running = true; running;) {

        if (getRequestQueueSize() != 0) {
            processRequests();
        }

        processAgents();

        cycleCount++;
        if (cycleCount < 0) cycleCount = 0;

        // sleep long enough so that the next cycle will only start
        // after config["SERVER_CYCLE_DURATION"] milliseconds
        gettimeofday(&timer_end, NULL);
        elapsed_time = ((timer_end.tv_sec - timer_start.tv_sec) * 1000000) +
                       (timer_end.tv_usec - timer_start.tv_usec);
        if ((cycle_duration - elapsed_time) > 0)
            usleep((unsigned int) (cycle_duration - elapsed_time));
        timer_start = timer_end;
    }
}

void CogServer::processRequests(void)
{
    Request* request;
    while ((request = popRequest()) != NULL) {
        request->execute();
        delete request;
    }
}

void CogServer::processAgents(void)
{
    std::vector<Agent*>::const_iterator it;
    for (it = agents.begin(); it != agents.end(); ++it) {
        Agent* agent = *it;
        if ((cycleCount % agent->frequency()) == 0)
            agent->run(this);
    }
}

bool CogServer::registerAgent(const std::string& id, AbstractFactory<Agent> const* factory)
{
    return Registry<Agent>::register_(id, factory);
}

bool CogServer::unregisterAgent(const std::string& id)
{
    destroyAllAgents(id);
    return Registry<Agent>::unregister(id);
}

std::list<const char*> CogServer::agentIds() const
{
    return Registry<Agent>::all();
}

Agent* CogServer::createAgent(const std::string& id, const bool start)
{
    Agent* a = Registry<Agent>::create(id);
    if (start) startAgent(a);
    return a; 
}

void CogServer::startAgent(Agent* agent)
{
    agents.push_back(agent);
}

void CogServer::stopAgent(Agent* agent)
{
    agents.erase(std::find(agents.begin(), agents.end(), agent));
}

void CogServer::destroyAgent(Agent *agent)
{
    stopAgent(agent);
    delete agent;
}

void CogServer::destroyAllAgents(const std::string& id)
{
    // place agents with classinfo().id == id at the end of the container
    std::vector<Agent*>::iterator last = 
        std::partition(agents.begin(), agents.end(),
                       std::tr1::bind(equal_to_id(), std::tr1::placeholders::_1, id));

    // save the agents that should be deleted on a temporary container
    std::vector<Agent*> to_delete(last, agents.end());

    // remove those agents from the main container
    agents.erase(last, agents.end());

    // delete the selected agents; NOTE: we must ensure that this is executed
    // after the 'agents.erase' call above, because the agent's destructor might
    // include a recursive call to destroyAllAgents
    std::for_each(to_delete.begin(), to_delete.end(), safe_deleter<Agent>());
}

bool CogServer::registerRequest(const std::string& name, AbstractFactory<Request> const* factory)
{
    return Registry<Request>::register_(name, factory);
}

bool CogServer::unregisterRequest(const std::string& name)
{
    return Registry<Request>::unregister(name);
}

Request* CogServer::createRequest(const std::string& name)
{
    return Registry<Request>::create(name);
}

const RequestClassInfo& CogServer::requestInfo(const std::string& name) const
{
    return static_cast<const RequestClassInfo&>(Registry<Request>::classinfo(name));
}

std::list<const char*> CogServer::requestIds() const
{
    return Registry<Request>::all();
}

long CogServer::getCycleCount()
{
    return cycleCount;
}

void CogServer::stop()
{
    running = false;
}

Request* CogServer::popRequest()
{

    Request* request;

    pthread_mutex_lock(&messageQueueLock);
    if (requestQueue.empty()) {
        request = NULL;
    } else {
        request = requestQueue.front();
        requestQueue.pop();
    }
    pthread_mutex_unlock(&messageQueueLock);

    return request;
}

void CogServer::pushRequest(Request* request)
{
    pthread_mutex_lock(&messageQueueLock);
    requestQueue.push(request);
    pthread_mutex_unlock(&messageQueueLock);
}

int CogServer::getRequestQueueSize()
{
    pthread_mutex_lock(&messageQueueLock);
    int size = requestQueue.size();
    pthread_mutex_unlock(&messageQueueLock);
    return size;
}

bool CogServer::loadModule(const std::string& filename)
{
    if (modules.find(filename) !=  modules.end()) {
        logger().info("Module \"%s\" is already loaded.", filename.c_str());
        return false;
    }

    // reset error
    dlerror();

    logger().info("Loading module \"%s\"", filename.c_str());
    void *dynLibrary = dlopen(filename.c_str(), RTLD_LAZY);
    const char* dlsymError = dlerror();
    if ((dynLibrary == NULL) || (dlsymError)) {
        logger().error("Unable to load module \"%s\": %s", filename.c_str(), dlsymError);
        return false;
    }

    // reset error
    dlerror();

    // search for id function
    Module::IdFunction* id_func = (Module::IdFunction*) dlsym(dynLibrary, Module::id_function_name());
    dlsymError = dlerror();
    if (dlsymError) {
        logger().error("Unable to find symbol \"opencog_module_id\": %s (module %s)", dlsymError, filename.c_str());
        return false;
    }

    // get and check module id
    const char *module_id = (*id_func)();
    if (module_id == NULL) {
        logger().error("Invalid module id (module \"%s\")", filename.c_str());
        return false;
    }

    // search for 'load' & 'unload' symbols
    Module::LoadFunction* load_func = (Module::LoadFunction*) dlsym(dynLibrary, Module::load_function_name());
    dlsymError = dlerror();
    if (dlsymError) {
        logger().error("Unable to find symbol \"opencog_module_load\": %s", dlsymError);
        return false;
    }

    Module::UnloadFunction* unload_func = (Module::UnloadFunction*) dlsym(dynLibrary, Module::unload_function_name());
    dlsymError = dlerror();
    if (dlsymError) {
        logger().error("Unable to find symbol \"opencog_module_unload\": %s", dlsymError);
        return false;
    }

    // load and init module
    Module* module = (Module*) (*load_func)();

    // store two entries in the module map:
    //    1: filename => <struct module data>
    //    2: moduleid => <struct module data>
    // we rely on the assumption that no module id will match the filename of
    // another module (and vice-versa). This is probably reasonable since most
    // module filenames should have a .dll or .so suffix, and module ids should
    // (by convention) be prefixed with its class namespace (i.e., "opencog::")
    std::string i = module_id;
    std::string f = filename;
    ModuleData mdata = {module, i, f, load_func, unload_func, dynLibrary};
    modules[i] = mdata;
    modules[f] = mdata;

    // after registration, call the module's init() method
    module->init();
 
    return true;
}

bool CogServer::unloadModule(const std::string& moduleId)
{
    logger().info("[CogServer] unloadModule(%s)", moduleId.c_str());
    ModuleMap::const_iterator it = modules.find(moduleId);
    if (it == modules.end()) {
        logger().info("[CogServer::unloadModule] module \"%s\" is not loaded.", moduleId.c_str());
        return false;
    }
    ModuleData mdata = it->second;

    // cache filename, id and handle
    std::string filename = mdata.filename;
    std::string id       = mdata.id;
    void*       handle   = mdata.handle;

    // invoke the module's unload function
    (*mdata.unloadFunction)(mdata.module);

    // erase the map entries (one with the filename as key, and one with the module
    // id as key
    modules.erase(filename);
    modules.erase(id);

    // unload dynamically loadable library
    logger().info("Unloading module \"%s\"", filename.c_str());

    dlerror(); // reset error
    if (dlclose(handle) != 0) {
        const char* dlsymError = dlerror();
        if (dlsymError) {
            logger().error("Unable to unload module \"%s\": %s", filename.c_str(), dlsymError);
            return false;
        }
    }

    return true;
}

CogServer::ModuleData CogServer::getModuleData(const std::string& moduleId)
{
    ModuleMap::const_iterator it = modules.find(moduleId);
    if (it == modules.end()) {
        logger().info("[CogServer::getModuleData] module \"%s\" was not found.", moduleId.c_str());
        ModuleData nulldata = {NULL, "", "", NULL, NULL, NULL};
        return nulldata;
    }
    return it->second;
}

Module* CogServer::getModule(const std::string& moduleId)
{
    return getModuleData(moduleId).module;
}

// Used for debug purposes on unit tests
void CogServer::unitTestServerLoop(int nCycles)
{
    for (int i = 0; (nCycles == 0) || (i < nCycles); ++i) {
        processRequests();
        processAgents();
        cycleCount++;
        if (cycleCount < 0) cycleCount = 0;
    }
}
