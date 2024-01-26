// Copyright 2023 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stddef.h>
#include <assert.h>
#include <dlfcn.h>
#include <dse/testing.h>
#include <dse/logger.h>
#include <dse/modelc/controller/controller.h>
#include <dse/modelc/controller/model_private.h>
#include <dse/modelc/model.h>


extern Controller* controller_object_ref(void);


extern ModelDesc* __model_gw_create__(ModelDesc* m);
extern int __model_gw_step__(ModelDesc* m, double* model_time, double stop_time);
extern void __model_gw_destroy__(ModelDesc* m);


int controller_load_model(ModelInstanceSpec* model_instance)
{
    assert(model_instance);
    ModelInstancePrivate* mip = model_instance->private;
    ControllerModel*      controller_model = mip->controller_model;
    assert(controller_model);
    const char* dynlib_filename = model_instance->model_definition.full_path;

    errno = 0;

    if (dynlib_filename) {
        log_notice("Loading dynamic model: %s ...", dynlib_filename);
        void* handle = dlopen(dynlib_filename, RTLD_NOW | RTLD_LOCAL);
        if (handle == NULL) {
            log_notice("ERROR: dlopen call: %s", dlerror());
            goto error_dl;
        }
        /* Load the model interface.*/
        controller_model->vtable.create = dlsym(handle, MODEL_CREATE_FUNC_NAME);
        log_notice("Loading symbol: %s ... %s", MODEL_CREATE_FUNC_NAME,
            controller_model->vtable.create ? "ok" : "not found");
        controller_model->vtable.step = dlsym(handle, MODEL_STEP_FUNC_NAME);
        log_notice("Loading symbol: %s ... %s", MODEL_STEP_FUNC_NAME,
            controller_model->vtable.step ? "ok" : "not found");
        controller_model->vtable.destroy =
            dlsym(handle, MODEL_DESTROY_FUNC_NAME);
        log_notice("Loading symbol: %s ... %s", MODEL_DESTROY_FUNC_NAME,
            controller_model->vtable.destroy ? "ok" : "not found");

    } else {
        if (dse_yaml_find_node(
                model_instance->model_definition.doc, "spec/runtime/gateway")) {
            log_notice("Using gateway symbols: ...");
            controller_model->vtable.create = __model_gw_create__;
            controller_model->vtable.step = __model_gw_step__;
            controller_model->vtable.destroy = __model_gw_destroy__;
        }
    }

    return 0;

error_dl:
    if (errno == 0) errno = EINVAL;
    log_error("Failed to load dynamic model!");
    return errno;
}


int controller_load_models(SimulationSpec* sim)
{
    assert(sim);
    Controller* controller = controller_object_ref();
    int         rc = 0;

    controller->simulation = sim;

    Adapter* adapter = controller->adapter;
    assert(adapter);

    ModelInstanceSpec* _instptr = sim->instance_list;
    while (_instptr && _instptr->name) {
        ModelInstancePrivate* mip = _instptr->private;
        AdapterModel*         am = mip->adapter_model;
        ControllerModel*      cm = mip->controller_model;

        am->adapter = adapter;
        am->model_uid = _instptr->uid;
        /* Set the UID based lookup for Adapter Model. */
        char hash_key[UID_KEY_LEN];
        snprintf(hash_key, UID_KEY_LEN - 1, "%d", _instptr->uid);
        hashmap_set(&adapter->models, hash_key, am);
        /* Load the Model. */
        errno = 0;
        rc = controller_load_model(_instptr);
        if (rc) {
            if (errno == 0) errno = EINVAL;
            log_error("controller_load_model() failed!");
            break;
        }
        /* Create/Setup the model. */
        if (cm->vtable.create == NULL && cm->vtable.step == NULL) {
            log_error("Model interface not complete!");
            log_error("  %s (%p)", MODEL_CREATE_FUNC_NAME, cm->vtable.create);
            log_error("  %s (%p)", MODEL_STEP_FUNC_NAME, cm->vtable.step);
            log_error("  %s (%p)", MODEL_DESTROY_FUNC_NAME, cm->vtable.destroy);
            break;
        }
        rc = modelc_model_create(sim, _instptr, &cm->vtable);
        if (rc) {
            if (errno == 0) errno = EINVAL;
            log_error("modelc_model_create() failed!");
            break;
        }
        /* Next instance? */
        _instptr++;
    }

    return rc;
}
