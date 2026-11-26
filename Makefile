CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -std=gnu11 -g -Iinclude

all: aiorch agents

aiorch: src/main.c src/orchestrator.c include/orchestrator.h
	$(CC) $(CFLAGS) -o aiorch src/main.c src/orchestrator.c

agents: sleeper failer wordcount analyst reporter

sleeper: agents/sleeper.c
	$(CC) $(CFLAGS) -o agents/sleeper agents/sleeper.c

failer: agents/failer.c
	$(CC) $(CFLAGS) -o agents/failer agents/failer.c

wordcount: agents/wordcount.c
	$(CC) $(CFLAGS) -o agents/wordcount agents/wordcount.c

analyst: agents/analyst.c
	$(CC) $(CFLAGS) -o agents/analyst agents/analyst.c

reporter: agents/reporter.c
	$(CC) $(CFLAGS) -o agents/reporter agents/reporter.c

clean:
	rm -f aiorch agents/sleeper agents/failer agents/wordcount
	rm -f agents/analyst agents/reporter
	rm -f logs/*.log reports/*.report status/*.status status/*.tmp
	rm -f reports/*.briefing reports/*.tmp messages/msg_*.txt messages/msg_*.txt.tmp
	rm -f data/analysis.txt data/analysis.txt.tmp data/briefing.txt data/briefing.txt.tmp

.PHONY: all agents clean
