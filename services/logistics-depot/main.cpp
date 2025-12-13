#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <random>
#include <string>
#include <memory>
#include <map>

#include "dds/dds.h"
#include "CombatMessages.h"

// OpenTelemetry SDK
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/sdk/resource/resource.h"

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;
namespace resource = opentelemetry::sdk::resource;

#define SERVICE_NAME "logistics-depot"

static volatile sig_atomic_t running = 1;

struct SupplyStock {
    int quantity = 100;
    int dispatched = 0;
    std::string depot;
};

std::map<std::string, SupplyStock> supplies = {
    {"AMMO", {100, 0, "DEPOT_A"}},
    {"FUEL", {200, 0, "DEPOT_A"}},
    {"MEDICAL", {50, 0, "DEPOT_B"}},
    {"FOOD", {150, 0, "DEPOT_C"}}
};

void init_tracer() {
    otlp::OtlpHttpExporterOptions opts;
    opts.url = "http://localhost:4318/v1/traces";

    auto exporter = otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));

    auto resource_attrs = resource::Resource::Create({
        {"service.name", SERVICE_NAME},
        {"service.version", "1.0.0"}
    });

    auto provider = trace_sdk::TracerProviderFactory::Create(std::move(processor), resource_attrs);
    trace_api::Provider::SetTracerProvider(std::move(provider));

    printf("[TRACING] OpenTelemetry initialized for %s\n", SERVICE_NAME);
}

void shutdown_tracer() {
    std::shared_ptr<trace_api::TracerProvider> none;
    trace_api::Provider::SetTracerProvider(none);
}

void handle_signal(int sig) { running = 0; }

void print_supply_status() {
    int total_stock = 0, total_dispatched = 0;

    printf("\n+==========================================+\n");
    printf("|       LOGISTICS DEPOT STATUS REPORT      |\n");
    printf("+==========================================+\n");

    for (auto& kv : supplies) {
        printf("| %-10s: %4d units (%s) sent: %d   |\n",
               kv.first.c_str(), kv.second.quantity,
               kv.second.depot.c_str(), kv.second.dispatched);
        total_stock += kv.second.quantity;
        total_dispatched += kv.second.dispatched;
    }

    printf("+==========================================+\n");
    printf("| TOTAL: %d in stock | %d dispatched        |\n",
           total_stock, total_dispatched);
    printf("+==========================================+\n\n");
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting logistics depot...\n", SERVICE_NAME);

    init_tracer();
    auto tracer = trace_api::Provider::GetTracerProvider()->GetTracer(SERVICE_NAME, "1.0.0");

    // DDS setup
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) { fprintf(stderr, "Failed to create participant!\n"); return 1; }

    dds_entity_t read_topic = dds_create_topic(participant, &combat_ReconReport_desc,
                                                "ReconReportTopic", NULL, NULL);
    dds_entity_t write_topic = dds_create_topic(participant, &combat_SupplyUpdate_desc,
                                                 "SupplyUpdateTopic", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 100);

    dds_entity_t reader = dds_create_reader(participant, read_topic, qos, NULL);
    dds_entity_t writer = dds_create_writer(participant, write_topic, qos, NULL);
    dds_delete_qos(qos);

    printf("[%s] DDS connected...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> supply_type_dis(0, 3);
    std::uniform_int_distribution<> quantity_dis(5, 25);

    const char* SUPPLY_TYPES[] = {"AMMO", "FUEL", "MEDICAL", "FOOD"};

    void* samples[10];
    dds_sample_info_t infos[10];
    for (int i = 0; i < 10; i++) samples[i] = combat_ReconReport__alloc();

    time_t last_report = time(NULL);

    printf("[%s] Logistics depot ready, processing recon reports...\n", SERVICE_NAME);

    while (running) {
        dds_return_t n = dds_take(reader, samples, infos, 10, 10);

        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;

                combat_ReconReport* report = (combat_ReconReport*)samples[i];

                // Extract trace context from DDS message header
                std::string trace_id_str = report->trace_ctx.trace_id ? report->trace_ctx.trace_id : "";
                std::string parent_span_str = report->trace_ctx.span_id ? report->trace_ctx.span_id : "";

                // Create parent span context
                trace_api::TraceId trace_id;
                trace_api::SpanId parent_span_id;

                if (trace_id_str.length() == 32) {
                    uint8_t trace_id_buf[16];
                    for (int j = 0; j < 16; j++) {
                        sscanf(trace_id_str.c_str() + j*2, "%2hhx", &trace_id_buf[j]);
                    }
                    trace_id = trace_api::TraceId(trace_id_buf);
                }

                if (parent_span_str.length() == 16) {
                    uint8_t span_id_buf[8];
                    for (int j = 0; j < 8; j++) {
                        sscanf(parent_span_str.c_str() + j*2, "%2hhx", &span_id_buf[j]);
                    }
                    parent_span_id = trace_api::SpanId(span_id_buf);
                }

                auto parent_ctx = trace_api::SpanContext(trace_id, parent_span_id,
                    trace_api::TraceFlags(trace_api::TraceFlags::kIsSampled), true);

                trace_api::StartSpanOptions opts;
                opts.parent = parent_ctx;

                auto span = tracer->StartSpan("dispatch-supplies", opts);
                auto scope = tracer->WithActiveSpan(span);

                // Determine supply type based on threat level
                const char* supply_type = SUPPLY_TYPES[supply_type_dis(gen)];
                int dispatch_qty = quantity_dis(gen);

                // Higher threat = more supplies needed
                std::string threat = report->threat_level ? report->threat_level : "LOW";
                if (threat == "HIGH" || threat == "EXTREME") {
                    dispatch_qty *= 2;
                }

                span->SetAttribute("mission.id", report->mission_id ? report->mission_id : "");
                span->SetAttribute("recon.threat_level", threat);
                span->SetAttribute("supply.type", supply_type);
                span->SetAttribute("supply.quantity", dispatch_qty);

                // Update supply inventory
                std::string supply_key = supply_type;
                if (supplies.find(supply_key) != supplies.end()) {
                    if (supplies[supply_key].quantity >= dispatch_qty) {
                        supplies[supply_key].quantity -= dispatch_qty;
                        supplies[supply_key].dispatched += dispatch_qty;
                    } else {
                        dispatch_qty = supplies[supply_key].quantity;
                        supplies[supply_key].quantity = 0;
                        supplies[supply_key].dispatched += dispatch_qty;
                    }
                }

                usleep(200000 + (rand() % 300000));

                span->SetAttribute("depot.location", supplies[supply_key].depot);
                span->SetAttribute("depot.remaining_stock", supplies[supply_key].quantity);

                auto span_context = span->GetContext();
                char new_span_id_hex[17] = {0};
                span_context.span_id().ToLowerBase16({new_span_id_hex, 16});

                bool low_stock = supplies[supply_key].quantity < 20;

                printf("[DISPATCH] %s x%d -> Mission %s | Stock: %d | trace: %.8s\n",
                       supply_type, dispatch_qty,
                       report->mission_id ? report->mission_id : "?",
                       supplies[supply_key].quantity, trace_id_str.c_str());

                if (low_stock) {
                    printf("[WARNING] Low stock alert for %s!\n", supply_type);
                    span->AddEvent("low_stock_warning");
                }

                span->SetStatus(trace_api::StatusCode::kOk);
                span->End();

                // Send supply update message via DDS
                combat_SupplyUpdate update;
                memset(&update, 0, sizeof(update));

                update.trace_ctx.trace_id = (char*)trace_id_str.c_str();
                update.trace_ctx.span_id = new_span_id_hex;
                update.trace_ctx.parent_span_id = (char*)parent_span_str.c_str();
                update.trace_ctx.trace_flags = 1;

                update.source_service = (char*)SERVICE_NAME;
                update.timestamp_ns = time(NULL) * 1000000000LL;
                update.mission_id = report->mission_id;
                update.supply_type = (char*)supply_type;
                update.action = (char*)"DISPATCH";
                update.depot_location = (char*)supplies[supply_key].depot.c_str();
                update.quantity = dispatch_qty;
                update.current_stock = supplies[supply_key].quantity;
                update.low_stock_alert = low_stock;

                dds_write(writer, &update);
            }
        }

        if (time(NULL) - last_report >= 20) {
            print_supply_status();
            last_report = time(NULL);
        }

        usleep(100000);
    }

    for (int i = 0; i < 10; i++) {
        dds_sample_free(samples[i], &combat_ReconReport_desc, DDS_FREE_ALL);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    shutdown_tracer();
    dds_delete(participant);
    return 0;
}
