# Variables de Compilacion
CC = gcc
CFLAGS = -Wall -Wextra -Isrc -O2
LIBS = -lrt -pthread

# Directorios
SRC_DIR = src
OBJ_DIR = obj

# Nombre del ejecutable final
TARGET = minisync

# Archivos fuente y objetos correspondientes
SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/scanner.c $(SRC_DIR)/worker.c $(SRC_DIR)/logger.c
OBJS = $(OBJ_DIR)/main.o $(OBJ_DIR)/scanner.o $(OBJ_DIR)/worker.o $(OBJ_DIR)/logger.o

# Regla por defecto: compilar todo
all: $(OBJ_DIR) $(TARGET)

# Crear el ejecutable enlazando los objetos compilados
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

# Compilar cada archivo .c a su archivo objeto .o dentro de obj/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Crear la carpeta de objetos temporales si no existe
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Limpiar ejecutables y objetos generados
clean:
	rm -rf $(OBJ_DIR) $(TARGET) sincronizacion.log backup

# Declarar objetivos que no representan archivos reales
.PHONY: all clean