.PHONY: up down logs clean rebuild status

# Tum servisleri baslat
up:
	docker compose up --build -d
	@echo ""
	@echo "🏭 Akilli Fabrika baslatildi!"
	@echo ""
	@echo "📊 Jaeger UI: http://localhost:16686"
	@echo ""
	@echo "Loglari gormek icin: make logs"

# Servisleri durdur
down:
	docker compose down

# Tum loglari goster
logs:
	docker compose logs -f

# Belirli bir servisin loglarini goster
logs-%:
	docker compose logs -f $*

# Temizlik
clean:
	docker compose down -v --rmi local
	docker system prune -f

# Yeniden derle ve baslat
rebuild:
	docker compose down
	docker compose build --no-cache
	docker compose up -d

# Sadece Jaeger'i baslat
jaeger:
	docker compose up -d jaeger
	@echo ""
	@echo "📊 Jaeger UI: http://localhost:16686"

# Durum kontrolu
status:
	docker compose ps
