.PHONY: up down logs clean rebuild status

# Tum servisleri baslat
up:
	docker compose up --build -d
	@echo ""
	@echo ""
	@echo "📊 Jaeger UI: http://localhost:16686"
	@echo ""
	@echo "To show logs: make logs"

# Stop services
down:
	docker compose down

# Show all logs
logs:
	docker compose logs -f

# Show log of a specific service.
logs-%:
	docker compose logs -f $*

# Cleanup
clean:
	docker compose down -v --rmi local
	docker system prune -f

# Rebuild and start
rebuild:
	docker compose down
	docker compose build --no-cache
	docker compose up -d

# Start jaeger only
jaeger:
	docker compose up -d tracing-jaeger
	@echo ""
	@echo "📊 Jaeger UI: http://localhost:16686"

# Status
status:
	docker compose ps
