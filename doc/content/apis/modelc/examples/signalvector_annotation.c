#include <stdlib.h>
#include <dse/modelc/model.h>

ModelDesc* model_create(ModelDesc* m)
{
    ModelSignalIndex idx = m->index(m, "data", "counter");

    if (idx.scalar) {
        /* Set initial value. */
        const char* v = idx.sv->annotation(idx.sv, idx.signal, "initial_value");
        if (v) *(idx.scalar) = atoi(v);
    }

    return m;
}
