// Copyright 2023 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#ifndef DSE_MODELC_ADAPTER_MESSAGE_H_
#define DSE_MODELC_ADAPTER_MESSAGE_H_

#include <stdint.h>
#include <dse_schemas/flatbuffers/simbus_channel_builder.h>
#include <dse/platform.h>


#undef ns
#define ns(x) FLATBUFFERS_WRAP_NAMESPACE(dse_schemas_fbs_channel, x)


/* message.c */
DLL_PRIVATE int32_t send_message(Adapter* adapter, void* endpoint_channel,
    uint32_t model_uid, ns(MessageType_union_ref_t) message, bool ack);
DLL_PRIVATE int32_t send_message_ack(Adapter* adapter, void* endpoint_channel,
    uint32_t model_uid, ns(MessageType_union_ref_t) message, int32_t token,
    int32_t rc, char* response);
DLL_PRIVATE int32_t wait_message(Adapter* adapter, const char** channel_name,
    ns(MessageType_union_type_t) message_type, int32_t token, bool* found);

typedef void (*HandleChannelMessageFunc)(Adapter* adapter,
    const char* channel_name, ns(ChannelMessage_table_t) channel_message,
    int32_t     token);


#endif  // DSE_MODELC_ADAPTER_MESSAGE_H_
