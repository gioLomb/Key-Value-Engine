CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11
TARGET = server

SRC = server.c hash_table.c
OBJ = $(SRC:.c=.o)

# .PHONY serve a dire a make che questi non sono file fisici
.PHONY: all clean run

# Compila e poi pulisce i file oggetto automaticamente
all: $(TARGET)
	@rm -f $(OBJ)
	@echo "Compilazione completata e file .o rimossi."

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

server.o: server.c hash_table.h
	$(CC) $(CFLAGS) -c server.c

hash_table.o: hash_table.c hash_table.h
	$(CC) $(CFLAGS) -c hash_table.c

# Nuovo comando per compilare ed eseguire subito
run: all
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)