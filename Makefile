all: smallchat-server smallchat-client
CFLAGS=-O2 -Wall -W -std=c99

smallchat-server: smallchat-server.c chatlib.c
	$(CC) smallchat-server.c chatlib.c -o smallchat-server $(CFLAGS)

smallchat-client: smallchat-client.c chatlib.c
	$(CC) smallchat-client.c chatlib.c -o smallchat-client $(CFLAGS)

# 客户端输入处理回归测试 (验证控制序列吞除/退格)
check:
	$(CC) -c -Dmain=client_main smallchat-client.c -o /tmp/client_input.o $(CFLAGS)
	$(CC) client_input_test.c /tmp/client_input.o chatlib.c -o /tmp/client_input_test $(CFLAGS)
	/tmp/client_input_test

clean:
	rm -f smallchat-server
	rm -f smallchat-client
