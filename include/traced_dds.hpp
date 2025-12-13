// DDS Tracing Library
// Automatic trace context injection/extraction - ZERO manual setup required!
//
// Configuration via environment variables:
//   TRACED_SERVICE_NAME - Service name for tracing (required)
//   OTEL_EXPORTER_OTLP_ENDPOINT - OTLP endpoint (default: http://localhost:4318/v1/traces)
//
// Usage:
//   auto writer = TRACED_WRITER(MsgType, participant, "TopicName");
//   auto reader = TRACED_READER(MsgType, participant, "TopicName");
//
//   // Publishing (auto-injects trace context)
//   writer.write(msg, "operation-name");
//
//   // Subscribing (auto-extracts and creates child span)
//   reader.take("span-name", [](auto& msg, auto& span) {
//       span.SetAttribute("key", "value");
//       // process msg...
//   });

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <cstdio>
#include <cstring>

#include "dds/dds.h"

#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/sdk/resource/resource.h"

namespace traced {

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;

// Global tracer instance
inline opentelemetry::nostd::shared_ptr<trace_api::Tracer> g_tracer;
inline std::string g_service_name;
inline bool g_initialized = false;

// Thread-local active trace context for automatic propagation
inline thread_local std::string g_active_trace_id;
inline thread_local std::string g_active_span_id;

namespace internal {

inline void do_init() {
    if (g_initialized) return;

    const char* service_name = getenv("TRACED_SERVICE_NAME");
    if (!service_name) service_name = "unknown-service";

    const char* otlp_endpoint = getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    if (!otlp_endpoint) otlp_endpoint = "http://localhost:4318/v1/traces";

    g_service_name = service_name;

    otlp::OtlpHttpExporterOptions opts;
    opts.url = otlp_endpoint;

    auto exporter = otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));

    auto res = resource::Resource::Create({
        {"service.name", g_service_name},
        {"service.version", "1.0.0"}
    });

    auto provider = trace_sdk::TracerProviderFactory::Create(std::move(processor), res);
    trace_api::Provider::SetTracerProvider(std::move(provider));

    g_tracer = trace_api::Provider::GetTracerProvider()->GetTracer(g_service_name, "1.0.0");
    g_initialized = true;

    printf("[traced] Initialized tracing for %s -> %s\n", g_service_name.c_str(), otlp_endpoint);
}

inline void do_shutdown() {
    if (!g_initialized) return;
    std::shared_ptr<trace_api::TracerProvider> none;
    trace_api::Provider::SetTracerProvider(none);
    g_initialized = false;
}

// Auto-initializer using static object
struct AutoInit {
    AutoInit() { do_init(); }
    ~AutoInit() { do_shutdown(); }
};

inline AutoInit& get_auto_init() {
    static AutoInit instance;
    return instance;
}

// Force initialization by referencing the static instance
inline bool ensure_init() {
    (void)get_auto_init();
    return g_initialized;
}

// Thread-local buffers for trace context strings
inline thread_local char trace_id_buf[33];
inline thread_local char span_id_buf[17];
inline thread_local char parent_span_buf[17];

inline void trace_id_to_hex(const trace_api::TraceId& id, char* out) {
    id.ToLowerBase16({out, 32});
    out[32] = '\0';
}

inline void span_id_to_hex(const trace_api::SpanId& id, char* out) {
    id.ToLowerBase16({out, 16});
    out[16] = '\0';
}

inline trace_api::TraceId hex_to_trace_id(const char* hex) {
    if (!hex || strlen(hex) != 32) return trace_api::TraceId();
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) sscanf(hex + i*2, "%2hhx", &buf[i]);
    return trace_api::TraceId(buf);
}

inline trace_api::SpanId hex_to_span_id(const char* hex) {
    if (!hex || strlen(hex) != 16) return trace_api::SpanId();
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) sscanf(hex + i*2, "%2hhx", &buf[i]);
    return trace_api::SpanId(buf);
}

// Trait to access trace_ctx field - specialize for your message types
template<typename T>
struct TraceContextAccessor {
    static auto& get(T& msg) { return msg.trace_ctx; }
    static const auto& get(const T& msg) { return msg.trace_ctx; }
};

} // namespace internal

// ============ Traced Writer ============

/**
 * Traced DDS Writer - automatically injects trace context on write
 */
template<typename T, typename Desc>
class Writer {
public:
    Writer(dds_entity_t participant, const char* topic_name, const Desc& desc) {
        internal::ensure_init();  // Auto-initialize tracing
        topic_ = dds_create_topic(participant, &desc, topic_name, nullptr, nullptr);

        dds_qos_t* qos = dds_create_qos();
        dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
        dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 100);

        writer_ = dds_create_writer(participant, topic_, qos, nullptr);
        dds_delete_qos(qos);
    }

    ~Writer() {
        // DDS cleanup handled by participant deletion
    }

    /**
     * Write message - automatically continues active trace or creates new root span
     */
    bool write(T& msg, const std::string& span_name) {
        opentelemetry::nostd::shared_ptr<trace_api::Span> span;

        // Check if there's an active trace context (set by Reader.take)
        if (!g_active_trace_id.empty() && !g_active_span_id.empty()) {
            // Continue the existing trace chain
            auto trace_id = internal::hex_to_trace_id(g_active_trace_id.c_str());
            auto parent_span = internal::hex_to_span_id(g_active_span_id.c_str());

            auto parent_ctx = trace_api::SpanContext(trace_id, parent_span,
                trace_api::TraceFlags(trace_api::TraceFlags::kIsSampled), true);

            trace_api::StartSpanOptions opts;
            opts.parent = parent_ctx;

            span = g_tracer->StartSpan(span_name, opts);
        } else {
            // No active trace - create root span
            span = g_tracer->StartSpan(span_name);
        }

        auto scope = g_tracer->WithActiveSpan(span);
        inject(msg, span);

        dds_return_t ret = dds_write(writer_, &msg);

        if (ret >= 0) {
            span->SetStatus(trace_api::StatusCode::kOk);
        } else {
            span->SetStatus(trace_api::StatusCode::kError, "DDS write failed");
        }
        span->End();

        return ret >= 0;
    }

    dds_entity_t get() { return writer_; }

private:
    void inject(T& msg, opentelemetry::nostd::shared_ptr<trace_api::Span>& span) {
        auto ctx = span->GetContext();
        internal::trace_id_to_hex(ctx.trace_id(), internal::trace_id_buf);
        internal::span_id_to_hex(ctx.span_id(), internal::span_id_buf);

        auto& tc = internal::TraceContextAccessor<T>::get(msg);
        tc.trace_id = internal::trace_id_buf;
        tc.span_id = internal::span_id_buf;
        tc.parent_span_id = (char*)"";
        tc.trace_flags = 1;
    }

    dds_entity_t topic_;
    dds_entity_t writer_;
};

// ============ Traced Reader ============

/**
 * Traced DDS Reader - automatically extracts trace context and creates child span
 */
template<typename T, typename Desc>
class Reader {
public:
    Reader(dds_entity_t participant, const char* topic_name, const Desc& desc) {
        internal::ensure_init();  // Auto-initialize tracing
        topic_ = dds_create_topic(participant, &desc, topic_name, nullptr, nullptr);

        dds_qos_t* qos = dds_create_qos();
        dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
        dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 100);

        reader_ = dds_create_reader(participant, topic_, qos, nullptr);
        dds_delete_qos(qos);

        // Pre-allocate sample buffers
        for (int i = 0; i < MAX_SAMPLES; i++) {
            samples_[i] = nullptr;  // DDS will allocate
        }
    }

    ~Reader() {
        // Samples freed by DDS or manually
    }

    /**
     * Take messages and process with callback
     * Callback receives: message and active span
     * Trace context is automatically propagated to any writer.write() calls within the callback
     */
    template<typename Callback>
    int take(const std::string& span_name, Callback&& callback) {
        dds_sample_info_t infos[MAX_SAMPLES];
        dds_return_t n = dds_take(reader_, samples_, infos, MAX_SAMPLES, MAX_SAMPLES);

        int processed = 0;
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;

                T* msg = static_cast<T*>(samples_[i]);

                // Extract trace context and create child span
                auto& tc = internal::TraceContextAccessor<T>::get(*msg);
                std::string trace_id_str = tc.trace_id ? tc.trace_id : "";
                std::string span_id_str = tc.span_id ? tc.span_id : "";

                auto trace_id = internal::hex_to_trace_id(trace_id_str.c_str());
                auto parent_span_id = internal::hex_to_span_id(span_id_str.c_str());

                auto parent_ctx = trace_api::SpanContext(trace_id, parent_span_id,
                    trace_api::TraceFlags(trace_api::TraceFlags::kIsSampled), true);

                trace_api::StartSpanOptions opts;
                opts.parent = parent_ctx;

                auto span = g_tracer->StartSpan(span_name, opts);
                auto scope = g_tracer->WithActiveSpan(span);

                // Set thread-local active trace context for automatic propagation
                auto ctx = span->GetContext();
                internal::trace_id_to_hex(ctx.trace_id(), internal::trace_id_buf);
                internal::span_id_to_hex(ctx.span_id(), internal::span_id_buf);
                g_active_trace_id = internal::trace_id_buf;
                g_active_span_id = internal::span_id_buf;

                // Call user callback with message and span
                callback(*msg, *span);

                // Clear active context after callback
                g_active_trace_id.clear();
                g_active_span_id.clear();

                // Auto-set OK status if not already set
                span->SetStatus(trace_api::StatusCode::kOk);
                span->End();
                processed++;
            }
        }

        return processed;
    }

    dds_entity_t get() { return reader_; }

private:
    static constexpr int MAX_SAMPLES = 10;

    dds_entity_t topic_;
    dds_entity_t reader_;
    void* samples_[MAX_SAMPLES];
};

// ============ Convenience Macros ============

// Register message type for tracing (put in header after including generated IDL header)
#define TRACED_DDS_TYPE(MsgType) \
    template<> \
    struct traced::internal::TraceContextAccessor<MsgType> { \
        static auto& get(MsgType& msg) { return msg.trace_ctx; } \
        static const auto& get(const MsgType& msg) { return msg.trace_ctx; } \
    }

// Create traced writer
#define TRACED_WRITER(MsgType, participant, topic_name) \
    traced::Writer<MsgType, decltype(MsgType##_desc)>(participant, topic_name, MsgType##_desc)

// Create traced reader
#define TRACED_READER(MsgType, participant, topic_name) \
    traced::Reader<MsgType, decltype(MsgType##_desc)>(participant, topic_name, MsgType##_desc)

} // namespace traced
