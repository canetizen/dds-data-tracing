#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <random>
#include <string>
#include <map>

#include "traced_dds.hpp"
#include "CombatMessages.h"

TRACED_DDS_TYPE(combat_ReconReport);
TRACED_DDS_TYPE(combat_SupplyUpdate);

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

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) { fprintf(stderr, "Failed to create participant!\n"); return 1; }

    auto reader = TRACED_READER(combat_ReconReport, participant, "ReconReportTopic");
    auto writer = TRACED_WRITER(combat_SupplyUpdate, participant, "SupplyUpdateTopic");

    printf("[%s] DDS connected...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> supply_type_dis(0, 3);
    std::uniform_int_distribution<> quantity_dis(5, 25);

    const char* SUPPLY_TYPES[] = {"AMMO", "FUEL", "MEDICAL", "FOOD"};

    time_t last_report = time(NULL);

    printf("[%s] Logistics depot ready, processing recon reports...\n", SERVICE_NAME);

    while (running) {
        reader.take("dispatch-supplies", [&](combat_ReconReport& report, traced::trace_api::Span& span) {
            const char* supply_type = SUPPLY_TYPES[supply_type_dis(gen)];
            int dispatch_qty = quantity_dis(gen);

            std::string threat = report.threat_level ? report.threat_level : "LOW";
            if (threat == "HIGH" || threat == "EXTREME") {
                dispatch_qty *= 2;
            }

            span.SetAttribute("mission.id", report.mission_id ? report.mission_id : "");
            span.SetAttribute("recon.threat_level", threat);
            span.SetAttribute("supply.type", supply_type);
            span.SetAttribute("supply.quantity", dispatch_qty);

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

            span.SetAttribute("depot.location", supplies[supply_key].depot);
            span.SetAttribute("depot.remaining_stock", supplies[supply_key].quantity);

            bool low_stock = supplies[supply_key].quantity < 20;

            printf("[DISPATCH] %s x%d -> Mission %s | Stock: %d\n",
                   supply_type, dispatch_qty,
                   report.mission_id ? report.mission_id : "?",
                   supplies[supply_key].quantity);

            if (low_stock) {
                printf("[WARNING] Low stock alert for %s!\n", supply_type);
                span.AddEvent("low_stock_warning");
            }

            // Send supply update
            combat_SupplyUpdate update;
            memset(&update, 0, sizeof(update));

            update.source_service = (char*)SERVICE_NAME;
            update.timestamp_ns = time(NULL) * 1000000000LL;
            update.mission_id = report.mission_id;
            update.supply_type = (char*)supply_type;
            update.action = (char*)"DISPATCH";
            update.depot_location = (char*)supplies[supply_key].depot.c_str();
            update.quantity = dispatch_qty;
            update.current_stock = supplies[supply_key].quantity;
            update.low_stock_alert = low_stock;

            // Forward - trace context automatically propagated by middleware
            writer.write(update, "send-supply-update");
        });

        if (time(NULL) - last_report >= 20) {
            print_supply_status();
            last_report = time(NULL);
        }

        usleep(100000);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);
    return 0;
}
