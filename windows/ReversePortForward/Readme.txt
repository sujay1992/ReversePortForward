ReversePortForward.exe                      # uses settings.txt in current directory
ReversePortForward.exe server_settings.txt  # custom settings file for server mode
ReversePortForward.exe client_settings.txt  # custom settings file for client mode


g++ -std=c++11 -O2 -pthread -o reverse_port_forward reverse_port_forward_linux.cpp

./reverse_port_forward                      # uses settings.txt in current directory
./reverse_port_forward server_settings.txt  # custom settings file for server mode
./reverse_port_forward client_settings.txt  # custom settings file for client mode