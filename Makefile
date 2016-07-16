# Fairly generic cross-compilation makefile for simple programs
CC=$(CROSSTOOL)/$(ARM)/bin/gcc
NAME=console

all: $(NAME)
	$(CROSSTOOL)/bin/$(ARM)-strip $(NAME)

$(NAME): $(NAME).c
