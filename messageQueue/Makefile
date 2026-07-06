CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall
LIBS = -lrt

TARGETS = mq_producer mq_consumer

all: $(TARGETS)

mq_producer: mq_producer.cpp common.h
	$(CXX) $(CXXFLAGS) -o mq_producer mq_producer.cpp $(LIBS)

mq_consumer: mq_consumer.cpp common.h
	$(CXX) $(CXXFLAGS) -o mq_consumer mq_consumer.cpp $(LIBS)

# Reset environment limits and clean old message queue states before run
reset_env:
	@echo "Raising system descriptor limits..."
	ulimit -n 4096 || true
	sudo sysctl -w fs.mqueue.queues_max=1024
	sudo rm -f /dev/mqueue/*

# Orchestrate execution: consumer runs in background, producer drives data path
run: all reset_env
	@echo "Starting IPC Benchmark Pipeline..."
	./mq_consumer & CONSUMER_PID=$$! ; \
	sleep 1 ; \
	./mq_producer ; \
	wait $$CONSUMER_PID
	@echo "[✓] Benchmark complete. Metrics saved to mq_results.csv"

clean:
	rm -f $(TARGETS) mq_results.csv
	sudo rm -f /dev/mqueue/*
