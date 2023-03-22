// Copyright 2023 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <dse/logger.h>
#include <dse/platform.h>
#include <dse/clib/collections/hashmap.h>
#include <dse/clib/util/strings.h>
#include <dse/clib/util/yaml.h>
#include <dse/modelc/adapter/transport/endpoint.h>
#include <dse/modelc/controller/controller.h>
#include <dse/modelc/controller/model_private.h>
#include <dse/modelc/model.h>

#define UNUSED(x)     ((void)x)
/* CLI related defaults. */
#define MODEL_TIMEOUT 60


static double __stop_request = 0; /* Very private, indicate stop request. */


static int _destroy_model_function(void* mf, void* additional_data)
{
    UNUSED(additional_data);

    /* This calls free() on the _mf object, add to hash with hashmap_set(). */
    model_function_destroy((ModelFunction*)mf);
    return 0;
}


static void _destroy_model_instances(SimulationSpec* sim)
{
    ModelInstanceSpec* _instptr = sim->instance_list;
    while (_instptr && _instptr->name) {
        ModelInstancePrivate* mip = _instptr->private;
        free(_instptr->name);
        free(_instptr->model_definition.full_path);
        /* ControllerModel */
        ControllerModel* cm = mip->controller_model;
        if (cm) {
            HashMap* mf_map = &cm->model_functions;
            hashmap_iterator(mf_map, _destroy_model_function, true, NULL);
            hashmap_destroy(mf_map);
            free(cm);
            cm = NULL;
        }
        /* Adapter Model. */
        AdapterModel* am = mip->adapter_model;
        if (am) {
            adapter_destroy_adapter_model(am);
            am = NULL;
        }

        /* Private data. */
        free(mip);

        /* Next instance? */
        _instptr++;
    }
    free(sim->instance_list);
}


int modelc_configure_model(
    ModelCArguments* args, ModelInstanceSpec* model_instance)
{
    assert(args);
    assert(model_instance);
    errno = 0;

    YamlNode*   mi_node;
    YamlNode*   md_doc;
    YamlNode*   node;
    const char* model_name = NULL;

    /* Model Instance: locate in stack. */
    mi_node = dse_yaml_find_node_in_seq_in_doclist(args->yaml_doc_list, "Stack",
        "spec/models", "name", model_instance->name);
    if (mi_node == NULL) {
        if (errno == 0) errno = EINVAL;
        log_error("Model Instance not found in Stack!");
        return errno;
    }
    model_instance->spec = mi_node;
    /* UID, if not set (0) will be assigned by SimBus. */
    if (model_instance->uid == 0) {
        node = dse_yaml_find_node(mi_node, "uid");
        if (node) model_instance->uid = strtoul(node->scalar, NULL, 10);
    }
    /* Name. */
    node = dse_yaml_find_node(mi_node, "model/name");
    if (node && node->scalar) {
        model_name = node->scalar;
    } else {
        if (errno == 0) errno = EINVAL;
        log_error("Model Definition not found!");
        return errno;
    }
    model_instance->model_definition.name = model_name;
    /* Path. */
    node = dse_yaml_find_node(mi_node, "model/metadata/annotations/path");
    if (node && node->scalar) {
        model_instance->model_definition.path = node->scalar;
        /* Load and add the Model Definition to the doc list. */
        char* md_file =
            dse_path_cat(model_instance->model_definition.path, "model.yaml");
        log_notice("Load YAML File: %s", md_file);
        args->yaml_doc_list = dse_yaml_load_file(md_file, args->yaml_doc_list);
        free(md_file);
    }

    /* Propagators list. */
    model_instance->propagators =
        dse_yaml_find_node(model_instance->spec, "propagators");

    /* Model Definition. */
    const char* selector[] = { "metadata/name" };
    const char* value[] = { model_name };
    md_doc = dse_yaml_find_doc_in_doclist(args->yaml_doc_list,
            "Model", selector, value, 1);
    if (md_doc) {
        model_instance->model_definition.doc = md_doc;
        /* Filename (of the dynlib) */
        const char* selectors[] = { "os", "arch" };
        const char* values[] = { PLATFORM_OS, PLATFORM_ARCH };
        YamlNode*   dl_node;

        dl_node = dse_yaml_find_node_in_seq(
            md_doc, "spec/runtime/dynlib", selectors, values, 2);
        if (dl_node) {
            node = dse_yaml_find_node(dl_node, "path");
            if (node && node->scalar) {
                model_instance->model_definition.file = node->scalar;
            }
        }
    }

    /* CLI overrides, development use case (normally take from stack). */
    if (args->file) model_instance->model_definition.file = args->file;
    if (args->path) model_instance->model_definition.path = args->path;

    /* Final checks. */
    if (model_instance->model_definition.file == NULL) {
        log_fatal("Model path not found in Model Definition!");
    }
    model_instance->model_definition.full_path =
        dse_path_cat(model_instance->model_definition.path,
            model_instance->model_definition.file);

    /* Set a reference ot the parsed YAML Doc List.  */
    model_instance->yaml_doc_list = args->yaml_doc_list;

    log_notice("Model Instance:");
    log_notice("  Name: %s", model_instance->name);
    log_notice("  UID: %u", model_instance->uid);
    log_notice("  Model Name: %s", model_instance->model_definition.name);
    log_notice("  Model Path: %s", model_instance->model_definition.path);
    log_notice("  Model File: %s", model_instance->model_definition.file);
    log_notice(
        "  Model Location: %s", model_instance->model_definition.full_path);

    return 0;
}


int modelc_configure(ModelCArguments* args, SimulationSpec* sim)
{
    assert(args);
    assert(sim);
    errno = 0;

    int model_count = 0;
    {
        char* model_names = strdup(args->name);
        char* _saveptr = NULL;
        char* _nameptr = strtok_r(model_names, ";", &_saveptr);
        while (_nameptr) {
            model_count++;
            _nameptr = strtok_r(NULL, ";", &_saveptr);
        }
        free(model_names);
    }
    log_trace("Parsed %d model names from %s", model_count, args->name);
    if (model_count == 0) {
        log_error("No model names parsed from arg (or stack): %s", args->name);
        errno = EINVAL;
        return -1;
    }
    sim->instance_list = calloc(model_count + 1, sizeof(ModelInstanceSpec));

    /* Configure the Simulation spec. */
    sim->transport = args->transport;
    sim->uri = args->uri;
    sim->uid = args->uid;
    sim->timeout = args->timeout;
    sim->step_size = args->step_size;
    sim->end_time = args->end_time;

    log_notice("Simulation Parameters:");
    log_notice("  Step Size: %f", sim->step_size);
    log_notice("  End Time: %f", sim->end_time);
    log_notice("  Model Timeout: %f", sim->timeout);

    log_notice("Transport:");
    log_notice("  Transport: %s", sim->transport);
    log_notice("  URI: %s", sim->uri);

    log_notice("Platform:");
    log_notice("  Platform OS: %s", PLATFORM_OS);
    log_notice("  Platform Arch: %s", PLATFORM_ARCH);

    /* Sanity-check any configuration. */
    if (sim->timeout <= 0) sim->timeout = MODEL_TIMEOUT;
    if (sim->step_size > sim->end_time) {
        log_fatal("Step Size is greater than End Time!");
    }

    /* Configure the Instance objects. */
    ModelInstanceSpec* _instptr = sim->instance_list;
    {
        char* model_names = strdup(args->name);
        char* _saveptr = NULL;
        char* _nameptr = strtok_r(model_names, ";", &_saveptr);
        while (_nameptr) {
            _instptr->private = calloc(1, sizeof(ModelInstancePrivate));
            ModelInstancePrivate* mip = _instptr->private;
            assert(_instptr->private);
            _instptr->name = strdup(_nameptr);
            modelc_configure_model(args, _instptr);

            /* Allocate a Controller Model object. */
            int rc;
            mip->controller_model = calloc(1, sizeof(ControllerModel));
            rc = hashmap_init(&mip->controller_model->model_functions);
            if (rc) {
                if (errno == 0) errno = ENOMEM;
                log_fatal("Hashmap init failed for model_functions!");
            }
            /* Allocate a Adapter Model object. */
            mip->adapter_model = calloc(1, sizeof(AdapterModel));
            rc = hashmap_init(&mip->adapter_model->channels);
            if (rc) {
                if (errno == 0) errno = ENOMEM;
                log_fatal("Hashmap init failed for channels!");
            }

            /* Next instance? */
            _nameptr = strtok_r(NULL, ";", &_saveptr);
            _instptr++;
        }
        free(model_names);
    }

    return 0;
}


static Endpoint* _create_endpoint(SimulationSpec* sim)
{
    int       retry_count = 60;
    Endpoint* endpoint = NULL;

    while (--retry_count) {
        endpoint = endpoint_create(
            sim->transport, sim->uri, sim->uid, false, sim->timeout);
        if (endpoint) break;
        if (__stop_request) {
            /* Early stop request, only would occur if endpoint creation
               was failing due to misconfiguration. */
            errno = ECANCELED;
            log_fatal("Signaled!");
        }
        sleep(1);
        log_notice("Retry endpoint creation ...");
    }

    return endpoint;
}


int modelc_run(SimulationSpec* sim, bool run_async)
{
    assert(sim);
    errno = 0;

    /* Create Endpoint object. */
    log_notice("Create the Endpoint object ...");
    Endpoint* endpoint = _create_endpoint(sim);
    if (endpoint == NULL) {
        log_fatal("Could not create endpoint!");
    }

    /* Setup UIDs. */
    if (sim->uid == 0) sim->uid = endpoint->uid;
    log_debug("sim->uid = %d", sim->uid);
    log_debug("endpoint->uid = %d", endpoint->uid);
    int                inst_counter = 0;
    ModelInstanceSpec* _instptr = sim->instance_list;
    while (_instptr && _instptr->name) {
        /* Generate a UID for this Model. */
        uint32_t _uid = (inst_counter * 10000) + sim->uid;
        if (_instptr->uid == 0) _instptr->uid = _uid;
        log_debug("mi[%d]->uid = %d", inst_counter, _instptr->uid);
        /* Next instance? */
        _instptr++;
        inst_counter++;
    }

    /* Create Controller object. */
    log_notice("Create the Controller object ...");
    controller_init(endpoint);

    /* Load all Simulation Models. */
    log_notice("Load and configure the Simulation Models ...");
    int rc = controller_load_models(sim);
    if (rc) log_fatal("Error loading Simulation Models!");

    /* Run async? */
    if (run_async == true) {
        log_notice("Setup for async Simulation Model run ...");
        controller_bus_ready(sim);
        return 0;
    }

    /* Otherwise, handover to the controller and do synchronous run. */
    log_notice("Run the Simulation ...");
    errno = 0;
    controller_run(sim);

    if (errno == ECANCELED) return errno;
    return 0; /* Caller can inspect errno to determine any conditions. */
}


int modelc_sync(SimulationSpec* sim)
{
    assert(sim);

    /* Async operation:
     * Models simulation environemnt manually syncs.
     * do_step() callbacks for each model function which activates, indicates
     * to models simulation environemnt _which_ model functions should run as
     * well as the start and stop times from the next step. */
    errno = 0;
    int rc = controller_step(sim);
    return rc;
}


void modelc_shutdown(void)
{
    /* Request and exit from the run loop.
       NOTICE: This is called from interrupt code, only set variables, then
       let controller_run() exit by itself.
    */
    __stop_request = 1;
    controller_stop();
}


void modelc_exit(SimulationSpec* sim)
{
    controller_dump_debug();
    controller_exit(sim);
    _destroy_model_instances(sim);
}
