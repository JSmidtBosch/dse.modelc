// Copyright 2023 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <dse/testing.h>
#include <dse/logger.h>
#include <dse/modelc/gateway.h>
#include <dse/modelc/schema.h>
#include <dse/modelc/controller/model_private.h>


#define UNUSED(x) ((void)x)


/* Gateway Model Functions
 * These represent the Model Interface of the Gateway. */

static HashList __mcd_list; /* Storage for ModelChannelDesc objects. */
static double   __gw_step_size;


static void* channel_spec_generator(ModelInstanceSpec* mi, void* data)
{
    UNUSED(mi);

    const char* name = dse_yaml_get_scalar((YamlNode*)data, "name");
    const char* alias = dse_yaml_get_scalar((YamlNode*)data, "alias");
    if (name || alias) {
        ChannelSpec* cs = calloc(1, sizeof(ChannelSpec));
        cs->name = name;
        cs->alias = alias;
        return cs; /* Caller to free. */
    }
    return NULL;
}


DLL_PRIVATE int __model_gw_step__(double* model_time, double stop_time)
{
    *model_time = stop_time;

    return 0;
}


DLL_PRIVATE int __model_gw_setup__(ModelInstanceSpec* mi)
{
    int rc;

    hashlist_init(&__mcd_list, 10);
    rc = model_function_register(
        mi, mi->name, __gw_step_size, __model_gw_step__);
    if (rc) log_fatal("Model registration failed!");

    uint32_t index = 0;
    do {
        /* Enumerate over all channels of the Model Instance (not the Model). */
        SchemaObject object = { .doc = mi->spec };
        ChannelSpec* cs = schema_object_enumerator(
            mi, &object, "channels", &index, channel_spec_generator);
        if (cs == NULL) break;

        /* Register this channel. Priority to 'alias' over 'name' (for channel)
         * as alias (if used) would match against a SignalGroup. Selector is
         * defined on the Model Instance and will match to a Label on the
         * signal group. */
        ModelChannelDesc* mcd = calloc(1, sizeof(ModelChannelDesc));
        mcd->name = cs->alias ? cs->alias : cs->name;
        mcd->function_name = mi->name;
        rc = model_configure_channel(mi, mcd);
        hashlist_append(&__mcd_list, mcd); /* Keep the mcd object. */
        free(cs);
    } while (1);

    return 0;
}


DLL_PRIVATE int __model_gw_exit__(ModelInstanceSpec* mi)
{
    UNUSED(mi);

    for (uint32_t i = 0; i < hashlist_length(&__mcd_list); i++) {
        void* o = hashlist_at(&__mcd_list, i);
        free(o);
    }
    hashlist_destroy(&__mcd_list);

    return 0;
}

/**
model_gw_setup
==============

Parameters
----------
gw (ModelGatewayDesc*)
: A gateway descriptor object, holds references to various ModelC objects.

name (const char*)
: Name of the gateway model. Used when parsing the provided YAML files to
  select the relevant configuration items (i.e. Model and SignalGroup schemas).

yaml_files (const char*)
: A list of YAML files where the relevant gateway configuration objects
  should be found.

log_level (int)
: The log level to apply to the gateway model. Common values include;
  LOG_NOTICE (default), LOG_INFO, LOG_QUIET (only errors) or LOG_DEBUG.
  Set to a negative number to use the default log level.

step_size (double)
: Step size for interactions with the Simbus.

end_time (double)
: End time for the simulation (acts as guard against "forever" simulations).

Returns
-------
0
: Success.

+ve
: Failure, inspect errno for the failing condition.

*/
int model_gw_setup(ModelGatewayDesc* gw, const char* name,
    const char** yaml_files, int log_level, double step_size, double end_time)
{
    int rc;

    assert(gw);

    /* Initialise the Gateway descriptor. */
    memset(gw, 0, sizeof(ModelGatewayDesc));
    gw->sim = calloc(1, sizeof(SimulationSpec));

    /* Construct the argument vector. */
    int          count = 0;
    const char** str_p = yaml_files;
    while (*str_p) {
        count++;
        str_p++;
    }
    size_t name_arg_len = 7 + strlen(name) + 1;
    gw->name_arg = calloc(name_arg_len, sizeof(char));
    snprintf(gw->name_arg, name_arg_len, "--name=%s", name);

    int argc = count + 2;
    gw->argv = calloc(argc, sizeof(char*));
    gw->argv[0] = "gateway"; /* The "executable" name/path. */
    gw->argv[1] = gw->name_arg;
    for (int i = 0; i < count; i++) {
        gw->argv[2 + i] = yaml_files[i];
    }
    log_info("Gateway arguments:");
    for (int i = 0; i < argc; i++) {
        log_info("  %s", gw->argv[i]);
    }

    /* Setup the ModelC library. */
    ModelCArguments args;
    modelc_set_default_args(&args, "gateway", step_size, end_time);
    args.log_level = log_level;
    modelc_parse_arguments(&args, argc, (char**)gw->argv, "Gateway");
    rc = modelc_configure(&args, gw->sim);
    if (rc) log_fatal("Unable to configure Model C!");

    /* Start the GW. */
    __gw_step_size = gw->sim->step_size;
    modelc_run(gw->sim, true); /* Calls __model_gw_setup__(). */

    /* Complete the Gateway descriptor. */
    gw->mi = modelc_get_model_instance(gw->sim, name);
    gw->sv = model_sv_create(gw->mi);

    return 0;
}


/**
model_gw_sync
=============

Parameters
----------
gw (ModelGatewayDesc*)
: A gateway descriptor object, holds references to various ModelC objects.

model_time (double)
: The current simulation time of the gateway model for which the
  Gateway API should synchronise with.

Returns
-------
0
: Success.

E_GATEWAYBEHIND
: The specified model_time is _behind_ the simulation time. The time should be
  advanced by the caller and then retry this call until the condition clears.

+ve
: Failure, inspect errno for the failing condition.

*/
int model_gw_sync(ModelGatewayDesc* gw, double model_time)
{
    ModelInstancePrivate* mip = gw->mi->private;

    /* If the gateway has fallen behind the SimBus time then the gateway
     * needs to advance its time (however it wishes) until this condition is
     * satisfied. Its not possible to advance the model time directly to the
     * same time as the SimBus time because we cannot be sure that the gateway
     * modelling environment will support that. */
    if (model_time < mip->adapter_model->model_time) return E_GATEWAYBEHIND;

    /* Advance the gateway as many times as necessary to reach the desired
     * model time. When this loop exits the gateway will be at the same time
     * as the SimBus time. After teh call to modelc_sync() the value in
     * mip->adapter_model->model_time will be the _next_ time to be used for
     * synchronisation with the SimBus - either within the while loop or on
     * the next call to model_gw_sync(). */
    while (mip->adapter_model->model_time <= model_time) {
        log_debug("GW steps the Model; model at %d, target is %d",
            mip->adapter_model->model_time, model_time);
        int rc = modelc_sync(gw->sim);
        if (rc) return rc;
    }

    return 0;
}


/**
model_gw_exit
=============

Terminates the Gateway Model and releases all objects referenced by the
ModelGatewayDesc object. The object itself is not affected and should be
released by the caller (if necessary).

Parameters
----------
gw (ModelGatewayDesc*)
: A gateway descriptor object, holds references to various ModelC objects.

Returns
-------
0
: Success.

+ve
: Failure, inspect errno for the failing condition.

*/
int model_gw_exit(ModelGatewayDesc* gw)
{
    if (gw == NULL) return 0;

    /* The doc-list can only be released _after_ modelc_exit() is called, but
     * in the process of calling modelc_exit() the gw->mi is destroyed,
     * therfore save a reference for later. */
    YamlDocList* yaml_dl = NULL;
    if (gw->mi) yaml_dl = gw->mi->yaml_doc_list;

    /* Exit the simulation and release all objects. */
    if (gw->sim) {
        modelc_exit(gw->sim);
        free(gw->sim);
    }
    if (gw->sv) model_sv_destroy(gw->sv);
    if (gw->name_arg) free(gw->name_arg);
    if (gw->argv) free(gw->argv);
    if (yaml_dl) dse_yaml_destroy_doc_list(yaml_dl);

    memset(gw, 0, sizeof(ModelGatewayDesc));
    return 0;
}
