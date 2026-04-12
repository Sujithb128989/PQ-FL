IMAGE_NAME ?= pqfl-server
SERVER_CONTAINER ?= pqfl-server-container

.PHONY: build test certs run bootstrap smoke status audit clean

build:
	docker build -t $(IMAGE_NAME) .

test:
	docker run --rm $(IMAGE_NAME) /app/pqfl_self_test

certs:
	mkdir -p certs
	docker run --rm -v "$$(pwd)/certs:/app/certs" $(IMAGE_NAME) /app/scripts/generate_certs.sh /app/certs

run:
	mkdir -p data models
	docker run -d --rm --name $(SERVER_CONTAINER) -p 50051:50051 \
		-v "$$(pwd)/certs:/app/certs:ro" \
		-v "$$(pwd)/data:/app/data" \
		-v "$$(pwd)/models:/app/models" \
		$(IMAGE_NAME)

bootstrap:
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		$(IMAGE_NAME) /app/pqfl_admin_demo

smoke:
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		-v "$$(pwd)/data:/app/data:ro" \
		$(IMAGE_NAME) /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		-v "$$(pwd)/data:/app/data:ro" \
		$(IMAGE_NAME) /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		-v "$$(pwd)/data:/app/data:ro" \
		$(IMAGE_NAME) /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		-v "$$(pwd)/data:/app/data:ro" \
		$(IMAGE_NAME) /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer

status:
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		$(IMAGE_NAME) /app/pqfl_admin_cli status

audit:
	docker run --rm --network container:$(SERVER_CONTAINER) \
		-v "$$(pwd)/certs:/app/certs:ro" \
		$(IMAGE_NAME) /app/pqfl_admin_cli audit 20

clean:
	-docker rm -f $(SERVER_CONTAINER)
