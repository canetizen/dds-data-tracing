#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
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

#define SERVICE_NAME "tactical-display"

static volatile sig_atomic_t running = 1;

struct CombatStats {
    int total_missions = 0;
    int targets_confirmed = 0;
    int targets_not_found = 0;
    int supplies_dispatched = 0;
    int alerts_generated = 0;
    std::map<std::string, int> by_zone;
    std::map<std::string, int> by_threat;
    time_t start_time;
} combat_stats;

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

void print_tactical_display() {
    int uptime = (int)(time(NULL) - combat_stats.start_time);
    float success_rate = combat_stats.targets_confirmed + combat_stats.targets_not_found > 0
        ? (float)combat_stats.targets_confirmed / (combat_stats.targets_confirmed + combat_stats.targets_not_found) * 100
        : 100.0;

    printf("\n");
    printf("+============================================================+\n");
    printf("|           TACTICAL COMMAND DISPLAY                         |\n");
    printf("+============================================================+\n");
    printf("|  Uptime: %6d seconds                                    |\n", uptime);
    printf("+------------------------------------------------------------+\n");
    printf("|  Total Missions:    %5d                                  |\n", combat_stats.total_missions);
    printf("|  Targets Confirmed: %5d   (%.1f%%)                        |\n", combat_stats.targets_confirmed, success_rate);
    printf("|  Targets Not Found: %5d                                  |\n", combat_stats.targets_not_found);
    printf("|  Supplies Sent:     %5d                                  |\n", combat_stats.supplies_dispatched);
    printf("|  Total Alerts:      %5d                                  |\n", combat_stats.alerts_generated);
    printf("+------------------------------------------------------------+\n");
    printf("|  Operations by Zone:                                       |\n");

    for (auto& kv : combat_stats.by_zone) {
        printf("|    %-8s: %5d missions                                 |\n",
               kv.first.c_str(), kv.second);
    }

    if (!combat_stats.by_threat.empty()) {
        printf("+------------------------------------------------------------+\n");
        printf("|  Threat Level Distribution:                                |\n");
        for (auto& kv : combat_stats.by_threat) {
            printf("|    %-10s: %3d                                         |\n",
                   kv.first.c_str(), kv.second);
        }
    }

    printf("+============================================================+\n\n");
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    combat_stats.start_time = time(NULL);

    printf("[%s] Starting tactical display system...\n", SERVICE_NAME);

    init_tracer();
    auto tracer = trace_api::Provider::GetTracerProvider()->GetTracer(SERVICE_NAME, "1.0.0");

    // DDS setup
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) { fprintf(stderr, "Failed to create participant!\n"); return 1; }

    dds_entity_t mission_topic = dds_create_topic(participant, &combat_MissionOrder_desc,
                                                "MissionOrderTopic", NULL, NULL);
    dds_entity_t recon_topic = dds_create_topic(participant, &combat_ReconReport_desc,
                                                   "ReconReportTopic", NULL, NULL);
    dds_entity_t supply_topic = dds_create_topic(participant, &combat_SupplyUpdate_desc,
                                                 "SupplyUpdateTopic", NULL, NULL);

    dds_qos_t* qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 100);

    dds_entity_t mission_reader = dds_create_reader(participant, mission_topic, qos, NULL);
    dds_entity_t recon_reader = dds_create_reader(participant, recon_topic, qos, NULL);
    dds_entity_t supply_reader = dds_create_reader(participant, supply_topic, qos, NULL);
    dds_delete_qos(qos);

    printf("[%s] DDS connected...\n", SERVICE_NAME);
    sleep(3);

    void* mission_samples[10];
    void* recon_samples[10];
    void* supply_samples[10];
    dds_sample_info_t infos[10];

    for (int i = 0; i < 10; i++) {
        mission_samples[i] = combat_MissionOrder__alloc();
        recon_samples[i] = combat_ReconReport__alloc();
        supply_samples[i] = combat_SupplyUpdate__alloc();
    }

    time_t last_display = time(NULL);

    printf("[%s] Tactical display ready, monitoring operations...\n", SERVICE_NAME);

    while (running) {
        // Process mission orders
        dds_return_t n = dds_take(mission_reader, mission_samples, infos, 10, 10);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;
                combat_MissionOrder* order = (combat_MissionOrder*)mission_samples[i];

                std::string trace_id_str = order->trace_ctx.trace_id ? order->trace_ctx.trace_id : "";
                std::string parent_span_str = order->trace_ctx.span_id ? order->trace_ctx.span_id : "";

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

                auto span = tracer->StartSpan("display-mission", opts);

                combat_stats.total_missions++;
                std::string zone = order->target_zone ? order->target_zone : "Unknown";
                combat_stats.by_zone[zone]++;

                span->SetAttribute("mission.type", order->mission_type ? order->mission_type : "");
                span->SetAttribute("mission.zone", zone);
                span->SetAttribute("display.total_missions", combat_stats.total_missions);

                printf("[DISPLAY] NEW MISSION: %s | Zone: %s | Priority: %s | trace: %.8s\n",
                       order->mission_type, zone.c_str(),
                       order->priority ? order->priority : "?",
                       trace_id_str.c_str());

                span->SetStatus(trace_api::StatusCode::kOk);
                span->End();
            }
        }

        // Process recon reports
        n = dds_take(recon_reader, recon_samples, infos, 10, 10);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;
                combat_ReconReport* report = (combat_ReconReport*)recon_samples[i];

                std::string trace_id_str = report->trace_ctx.trace_id ? report->trace_ctx.trace_id : "";
                std::string parent_span_str = report->trace_ctx.span_id ? report->trace_ctx.span_id : "";

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

                auto span = tracer->StartSpan("display-intel", opts);

                if (report->target_confirmed) {
                    combat_stats.targets_confirmed++;
                } else {
                    combat_stats.targets_not_found++;
                }

                std::string threat = report->threat_level ? report->threat_level : "UNKNOWN";
                combat_stats.by_threat[threat]++;

                span->SetAttribute("recon.target_confirmed", report->target_confirmed);
                span->SetAttribute("recon.threat_level", threat);
                span->SetAttribute("recon.enemy_count", report->enemy_count);

                printf("[DISPLAY] INTEL: %s | Threat: %s | Enemies: %d | trace: %.8s\n",
                       report->target_confirmed ? "TARGET CONFIRMED" : "NOT FOUND",
                       threat.c_str(), report->enemy_count, trace_id_str.c_str());

                if (threat == "EXTREME" || threat == "HIGH") {
                    combat_stats.alerts_generated++;
                    printf("\n[ALERT] High threat detected: %s level!\n\n", threat.c_str());
                    span->AddEvent("high_threat_alert");
                }

                span->SetStatus(trace_api::StatusCode::kOk);
                span->End();
            }
        }

        // Process supply updates
        n = dds_take(supply_reader, supply_samples, infos, 10, 10);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!infos[i].valid_data) continue;
                combat_SupplyUpdate* update = (combat_SupplyUpdate*)supply_samples[i];

                std::string trace_id_str = update->trace_ctx.trace_id ? update->trace_ctx.trace_id : "";
                std::string parent_span_str = update->trace_ctx.span_id ? update->trace_ctx.span_id : "";

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

                auto span = tracer->StartSpan("display-logistics", opts);

                combat_stats.supplies_dispatched += update->quantity;

                span->SetAttribute("supply.type", update->supply_type ? update->supply_type : "");
                span->SetAttribute("supply.quantity", update->quantity);
                span->SetAttribute("depot.stock", update->current_stock);

                printf("[DISPLAY] SUPPLY: %s x%d from %s | Stock: %d | trace: %.8s\n",
                       update->supply_type, update->quantity,
                       update->depot_location ? update->depot_location : "?",
                       update->current_stock, trace_id_str.c_str());

                if (update->low_stock_alert) {
                    combat_stats.alerts_generated++;
                    printf("\n[WARNING] Low stock for %s at %s!\n\n",
                           update->supply_type, update->depot_location);
                    span->AddEvent("low_stock_alert");
                }

                span->SetStatus(trace_api::StatusCode::kOk);
                span->End();
            }
        }

        if (time(NULL) - last_display >= 25) {
            print_tactical_display();
            last_display = time(NULL);
        }

        usleep(100000);
    }

    for (int i = 0; i < 10; i++) {
        dds_sample_free(mission_samples[i], &combat_MissionOrder_desc, DDS_FREE_ALL);
        dds_sample_free(recon_samples[i], &combat_ReconReport_desc, DDS_FREE_ALL);
        dds_sample_free(supply_samples[i], &combat_SupplyUpdate_desc, DDS_FREE_ALL);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    shutdown_tracer();
    dds_delete(participant);
    return 0;
}
