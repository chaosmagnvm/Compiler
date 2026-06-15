CXX      := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Iinc
TARGET   := compiler

# Автоматически находим все .cpp файлы в src/
SRCS := $(shell find src -name "*.cpp")
OBJS := $(patsubst src/%.cpp, build/%.o, $(SRCS))

# -------------------------------------------------------
# build: собрать компилятор
# -------------------------------------------------------
.PHONY: build
build: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Компилируем каждый .cpp в соответствующий .o
build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# -------------------------------------------------------
# run: собрать и запустить main.csm
# -------------------------------------------------------
.PHONY: run
run: build
	./$(TARGET) main.csm && gcc main.s -o main -no-pie && ./main

# -------------------------------------------------------
# debug: пересобрать с отладочной информацией
# -------------------------------------------------------
.PHONY: debug
debug: CXXFLAGS += -g -O0 -DDEBUG
debug: clean build

# -------------------------------------------------------
# clean: удалить артефакты
# -------------------------------------------------------
.PHONY: clean
clean:
	rm -rf build $(TARGET)
