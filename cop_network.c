//
//  scr-network.c
//  
//
//  Created by gwen on 13/05/2017.
//
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // for close()

#include <arpa/inet.h>
#include <sys/socket.h>

#include "cop_network.h"

static int receive_udp_socket = -1;
static int receive_tcp_socket = -1;

static int proxy_receive_udp_socket_cam = -1;
static int proxy_receive_udp_socket_mic = -1;
static bool is_network_running_cam = false;
static bool is_network_running_mic = false;
static FILE* file_cam = NULL;
static char* file_cam_name = NULL;
static FILE* file_mic = NULL;
static char* file_mic_name = NULL;

// Set by main() args
static const char* encryption_pwd_cam = NULL;
static const char* encryption_pwd_mic = NULL;

static const char* LOCALHOST_IP = "127.0.0.1";

// TODO: Before editing or using the list clone it (immutable)
struct list_item* client_data_cam_list = NULL;
struct list_item* client_data_mic_list = NULL;

typedef struct FileItem {
    char* file_name;
    long file_size_kb;
} FileItem;

command_data* network_receive_udp(int listen_port) {
    cop_debug("[network_receive_udp].");

    struct sockaddr_in addr, si_other;
    receive_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receive_udp_socket == -1) {
        cop_error("[network_receive_udp] Could not create socket.");
        return NULL;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    cop_debug("[network_receive_udp] Bind to port %d.", listen_port);
    int result = bind(receive_udp_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -1) {
        cop_error("[network_receive_udp] Could not bind socket do %d.", listen_port);
        return NULL;
    }

    char* buffer = malloc(sizeof(char) * BUFFER_SIZE);
    memset(buffer, '\0', BUFFER_SIZE);
    unsigned slen=sizeof(addr);
    recvfrom(receive_udp_socket, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&si_other, &slen);
    cop_debug("[network_receive_udp] Received: %s.", buffer);
    close(receive_udp_socket);

    char* token;

    command_data* data = malloc(sizeof(command_data));
    int i = 0;

    if (!contains(buffer, " ")) {
        data->cmd = buffer;
        return data;
    }

    while ((token = strsep(&buffer, " ")) != NULL) {
        cop_debug("Token: %s", token);
        if (i == 0) {
            data->cmd = token;
            i++;
            continue;
        }
        // CONNECT UDP <ip> <port-cam> <port-mic>
        if (equals(data->cmd, "CONNECT")) {
            if (i == 1) {
                data->protocol = token;
                i++;
                continue;
            }
            if (i == 2) {
                data->ip = token;
                i++;
                continue;
            }
            if (i == 3) {
                data->port_cam = str_to_int(token);
                i++;
                continue;
            }
            if (i == 4) {
                data->port_mic = str_to_int(token);
                i++;
                continue;
            }
        }
        if (equals(data->cmd, "DELETE")) {
            if (i == 1) {
                data->file_name = token;
                i++;
                continue;
            }
        }
        i++;
    }

    return data;
}

static void network_send_tcp(const void *data, size_t size, client_data* client_data) {

    cop_debug("[network_send_tcp] Send data to %s with length: %zu.", client_data->src_ip, size);

    int send_tcp_socket; 
    struct sockaddr_in serv_addr; 
  
    // socket create and varification 
    send_tcp_socket = socket(AF_INET, SOCK_STREAM, 0); 
    if (send_tcp_socket < 0) {
        cop_error("[network_send_tcp] Could not create socket.");
        return;
    }

    cop_debug("[network_send_tcp] Socket successfully created.");
    bzero(&serv_addr, sizeof(serv_addr)); 
  
    // assign IP, PORT 
    serv_addr.sin_family = AF_INET; 
    serv_addr.sin_addr.s_addr = inet_addr(client_data->src_ip); 
    serv_addr.sin_port = htons(PORT_LISTEN_SERVER); 
  
    // connect the client socket to server socket 
    if (connect(send_tcp_socket, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) != 0) { 
        cop_error("[network_send_tcp] Connection with the server failed."); 
        return;
    }
  
    // function for chat 
    int result = send(send_tcp_socket, data, size, 0);
    if (result < 0) {
        cop_error("[network_send_tcp] Send failed: %d.", result);
        return;
    }

    close(send_tcp_socket); 
}

/*
 * 0 = IDLE
 * 1 = INITIALIZING
 * 2 = CONNECTED;ip-send-to
 * 3 = DISCONNECTING
 * x = UNKNOWN
 */
static const char* get_state_str() {
    if (state == 0) {
        return "IDLE";
    } else if (state == 1) {
        return "INITIALIZING";
    } else if (state == 2) {
        const char* ipSendTo = get_sendto_ip();
        return concat("CONNECTED;", ipSendTo);
    } else if (state == 3) {
        return "DISCONNECTING";
    }
    return "UNKNOWN";
}

void network_send_state(const char* senderId) {

    // Camera
    for (int i = 0; i < list_length(client_data_cam_list); i++) {
        const char* msg = "STATE";
        msg = concat(msg, " ");
        msg = concat(msg, senderId);
        msg = concat(msg, " ");
        msg = concat(msg, get_state_str());
        msg = concat(msg, " ");
        msg = concat(msg, "CAM");
        size_t msg_length = strlen(msg);
        
        list_item* item = list_get(client_data_cam_list, i);
        client_data* data = (client_data*)item->data;
        network_send_tcp(msg, msg_length, data);
    }

    // Mic
    for (int i = 0; i < list_length(client_data_mic_list); i++) {
        const char* msg = "STATE";
        msg = concat(msg, " ");
        msg = concat(msg, senderId);
        msg = concat(msg, " ");
        msg = concat(msg, get_state_str());
        msg = concat(msg, " ");
        msg = concat(msg, "MIC");
        size_t msg_length = strlen(msg);
        
        list_item* item = list_get(client_data_mic_list, i);
        client_data* data = (client_data*)item->data;
        network_send_tcp(msg, msg_length, data);
    }
}

void proxy_close() {
    // TODO: Close sockets from list 2x
    if (proxy_receive_udp_socket_cam < 0) {
        cop_error("[proxy_close] cam: Socket receive not open: %d.", proxy_receive_udp_socket_cam);
    } else {
        cop_debug("[proxy_close] cam: Close 'receive-udp-socket'.");
        close(proxy_receive_udp_socket_cam);
    }
    is_network_running_cam = false;
    if (proxy_receive_udp_socket_mic < 0) {
        cop_error("[proxy_close] mic: Socket receive not open: %d.", proxy_receive_udp_socket_mic);
    } else {
        cop_debug("[proxy_close] mic: Close 'receive-udp-socket'.");
        close(proxy_receive_udp_socket_mic);
    }
    is_network_running_mic = false;
}

void server_close() {
    if (receive_tcp_socket < 0) {
        cop_error("[proxy_close] Socket receive (tcp) not open: %d.", receive_tcp_socket);
    } else {
        cop_debug("[proxy_close] Close 'receive-tcp-socket'.");
        close(receive_tcp_socket);
    }
}

static void set_next_file_cam() {
    file_cam_name = "video_";
    file_cam_name = concat(file_cam_name, get_timestamp());
    file_cam_name = concat(file_cam_name, ".ts");
    cop_debug("[set_next_file_cam] New video file: %s.", file_cam_name);
    file_cam = fopen(file_cam_name, "ab");
}

static void set_next_file_mic() {
    file_mic_name = "audio_";
    file_mic_name = concat(file_mic_name, get_timestamp());
    file_mic_name = concat(file_mic_name, ".ts");
    cop_debug("[set_next_file_mic] New audio file: %s.", file_mic_name);
    file_mic = fopen(file_mic_name, "ab");
}

static list_item* add_ip_to_list(list_item* client_data_list, char* ip, int dest_port) {
    bool exists = false;
    for (int i = 0; i < list_length(client_data_list); i++) {
        list_item* item = list_get(client_data_list, i);
        client_data* data = (client_data*)item->data;
        exists = equals(data->src_ip, ip) && data->src_port == dest_port;
        if (exists) {
            break;
        }
    }
    if (!exists) {
        cop_debug("[add_ip_to_list] Ip does not exists yet: %s. Add to list.", ip);
        // Add client data
        client_data* data = malloc(sizeof(client_data));
        data->src_ip = strdup(ip);
        data->src_port = dest_port;
        int client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket < 0) {
            cop_error("[add_ip_to_list] Could not create socket: %d.", socket);
            return;
        }
        data->socket = client_socket;
        client_data_list = list_push(client_data_list, data);
    } else {
        cop_debug("[add_ip_to_list] Ip already exists: %s. Do nothing.", ip);
    }
    return client_data_list;
}

void proxy_init_cam(char* dest_ip, int dest_port, const char* pwd) {
    cop_debug("[proxy_init_cam] %s %d.", dest_ip, dest_port);
    // TODO: Not needed anymore?
    client_data_cam_list = add_ip_to_list(client_data_cam_list, dest_ip, dest_port);
    encryption_pwd_cam = pwd;
    set_next_file_cam();

    is_network_running_cam = true;

    cop_debug("[proxy_init_cam] Done.");
}

void proxy_init_mic(char* dest_ip, int dest_port, const char* pwd) {
    cop_debug("[proxy_init_mic] %s %d.", dest_ip, dest_port);
    // TODO: Not needed anymore?
    client_data_mic_list = add_ip_to_list(client_data_mic_list, dest_ip, dest_port);
    encryption_pwd_mic = pwd;
    set_next_file_mic();

    is_network_running_mic = true;

    cop_debug("[proxy_init_mic] Done.");
}

// We only want to redirect output to localhost again
void proxy_reset_cam() {
    cop_debug("[proxy_reset] Resetting proxy to localhost and port %d.", PORT_PROXY_DESTINATION_DUMMY_CAM);
    for (int i = 0; i < list_length(client_data_cam_list); i++) {
        client_data_cam_list = list_delete(client_data_cam_list, 0);
    }
    // Add client data
    client_data* data = malloc(sizeof(client_data));
    data->src_ip = strdup(LOCALHOST_IP);
    data->src_port = PORT_PROXY_DESTINATION_DUMMY_CAM;
    client_data_cam_list = list_push(client_data_cam_list, data);
    cop_debug("[proxy_reset] Done.");
}
void proxy_reset_mic() {
    cop_debug("[proxy_reset] Resetting proxy to localhost and port %d.", PORT_PROXY_DESTINATION_DUMMY_MIC);
    for (int i = 0; i < list_length(client_data_mic_list); i++) {
        client_data_mic_list = list_delete(client_data_mic_list, 0);
    }
    // Add client data
    client_data* data = malloc(sizeof(client_data));
    data->src_ip = strdup(LOCALHOST_IP);
    data->src_port = PORT_PROXY_DESTINATION_DUMMY_MIC;
    client_data_mic_list = list_push(client_data_mic_list, data);
    cop_debug("[proxy_reset] Done.");
}

void proxy_connect_cam(const char* dest_ip, int dest_port) {
    cop_debug("[proxy_connect_cam] Connect proxy to %s and port %d.", dest_ip, dest_port);
    // Add client data
    client_data* data = malloc(sizeof(client_data));
    data->src_ip = strdup(dest_ip);
    data->src_port = dest_port;
    client_data_cam_list = list_push(client_data_cam_list, data);
    cop_debug("[proxy_connect_cam] Done. Length: %d.", list_length(client_data_cam_list));
}
void proxy_connect_mic(const char* dest_ip, int dest_port) {
    cop_debug("[proxy_connect_mic] Connect proxy to %s and port %d.", dest_ip, dest_port);
    // Add client data
    client_data* data = malloc(sizeof(client_data));
    data->src_ip = strdup(dest_ip);
    data->src_port = dest_port;
    client_data_mic_list = list_push(client_data_mic_list, data);
    cop_debug("[proxy_connect_mic] Done.");
}

// Type = 0: cam, 1: mic
void proxy_send_udp(int type, const char* data) {

    static struct sockaddr_in dest_addr;

    if (type == 0) {
        for (int i = 0; i < list_length(client_data_cam_list); i++) {
            list_item* item = list_get(client_data_cam_list, i);
            client_data* cl_data = (client_data*)item->data;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_addr.s_addr = inet_addr(cl_data->src_ip);
            dest_addr.sin_port = htons(cl_data->src_port);
            if (cl_data->socket < 0) {
                cop_error("[proxy_send_udp] cam: Socket not available: %d", cl_data->socket);
            }
            int result = sendto(cl_data->socket, data, PROXY_SEND_BUFFER_SIZE_BYTES, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (result < 0) {
                cop_error("[proxy_send_udp] cam: Could not send data. Result: %d.", result);
            }
        }
    } else {
        for (int i = 0; i < list_length(client_data_mic_list); i++) {
            list_item* item = list_get(client_data_mic_list, i);
            client_data* cl_data = (client_data*)item->data;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_addr.s_addr = inet_addr(cl_data->src_ip);
            dest_addr.sin_port = htons(cl_data->src_port);
            if (cl_data->socket < 0) {
                cop_error("[proxy_send_udp] mic: Socket not available: %d", cl_data->socket);
            }
            int result = sendto(cl_data->socket, data, PROXY_SEND_BUFFER_SIZE_BYTES, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (result < 0) {
                cop_error("[proxy_send_udp] mic: Could not send data. Result: %d.", result);
            }
        }
    }
}

// Type = 0: cam, 1: mic
static int proxy_receive_udp(int type, int proxy_receive_udp_socket, int port_proxy_listen) {

    struct sockaddr_in addr, si_other;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_proxy_listen);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    cop_debug("[proxy_receive_udp] Bind to port %d.", port_proxy_listen);
    int result = bind(proxy_receive_udp_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (result == -1) {
        cop_error("[proxy_receive_udp] Could not bind socket do %d.", port_proxy_listen);
        return STATUS_CODE_NOK;
    }

    char* sendBuffer = malloc(sizeof(char) * PROXY_SEND_BUFFER_SIZE_BYTES);
    memset(sendBuffer, '\0', PROXY_SEND_BUFFER_SIZE_BYTES);
    int sendIndex = 0;

    char* buffer = malloc(sizeof(char) * PROXY_BUFFER_SIZE_BYTES);
    memset(buffer, '\0', PROXY_BUFFER_SIZE_BYTES);
    unsigned slen=sizeof(addr);

    while ((type == 0 && is_network_running_cam) || (type == 1 && is_network_running_mic)) {
        int read = recvfrom(proxy_receive_udp_socket, buffer, PROXY_BUFFER_SIZE_BYTES, 0, (struct sockaddr *)&si_other, &slen);

        if (read == -1) {
            cop_error("[proxy_receive_udp] Stop proxy.");
            break;
        }

        if (sendIndex + read < PROXY_SEND_BUFFER_SIZE_BYTES) {
            // Buffer won't be filled
            memcpy(&sendBuffer[sendIndex], buffer, read);
            sendIndex += read;
        } else {
            // Buffer is filled
            memcpy(&sendBuffer[sendIndex], buffer, PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex);

            // Write to file
            if (type == 0 && file_cam != NULL) {
                fwrite(sendBuffer, PROXY_SEND_BUFFER_SIZE_BYTES, 1, file_cam);
                long size_in_kb = ftell(file_cam) / 1024;
                // Set max size to 250 mb
                //if (size_in_kb > 1024 * 250) {
                if (size_in_kb > 1024 * 100) {
                    fclose(file_cam);
                    set_next_file_cam();
                }
            } else if (type == 1 && file_mic != NULL) {
                fwrite(sendBuffer, PROXY_SEND_BUFFER_SIZE_BYTES, 1, file_mic);
                long size_in_kb = ftell(file_mic) / 1024;
                // Set max size to 250 mb
                //if (size_in_kb > 1024 * 250) {
                if (size_in_kb > 1024 * 100) {
                    fclose(file_mic);
                    set_next_file_mic();
                }
            }

            // Do encryption
            if (type == 0 && encryption_pwd_cam != NULL) {
                size_t pwd_length = strlen(encryption_pwd_cam);
                if (pwd_length > 0) {
                    for(int i = 0; i < PROXY_SEND_BUFFER_SIZE_BYTES; i++) {
                        sendBuffer[i] = sendBuffer[i] ^ encryption_pwd_cam[i % pwd_length];
                    }
                }
            } else if (type == 1 && encryption_pwd_mic != NULL) {
                size_t pwd_length = strlen(encryption_pwd_mic);
                if (pwd_length > 0) {
                    for(int i = 0; i < PROXY_SEND_BUFFER_SIZE_BYTES; i++) {
                        sendBuffer[i] = sendBuffer[i] ^ encryption_pwd_mic[i % pwd_length];
                    }
                }
            }

            proxy_send_udp(type, sendBuffer);
            memcpy(sendBuffer, &buffer[PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex], read - (PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex));
            sendIndex = read - (PROXY_SEND_BUFFER_SIZE_BYTES - sendIndex);
        }
    }

    if (file_cam != NULL) {
        fclose(file_cam);
    }
    file_cam_name = NULL;
    file_cam = NULL;

    if (file_mic != NULL) {
        fclose(file_mic);
    }
    file_mic_name = NULL;
    file_mic = NULL;
    
    cop_debug("[proxy_receive_udp] Done.");

    return STATUS_CODE_OK;
}

int proxy_receive_udp_cam(void* arg) {
    proxy_receive_udp_socket_cam = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_receive_udp_socket_cam == -1) {
        cop_error("[proxy_receive_udp_cam] Could not create socket.");
        return STATUS_CODE_NOK;
    }
    return proxy_receive_udp(0, proxy_receive_udp_socket_cam, PORT_PROXY_LISTEN_CAM);
}

int proxy_receive_udp_mic(void* arg) {
    proxy_receive_udp_socket_mic = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (proxy_receive_udp_socket_mic == -1) {
        cop_error("[proxy_receive_udp_mic] Could not create socket.");
        return STATUS_CODE_NOK;
    }
    return proxy_receive_udp(1, proxy_receive_udp_socket_mic, PORT_PROXY_LISTEN_MIC);
}

char* get_video_file_name() {
    return file_cam_name;
}

char* get_audio_file_name() {
    return file_mic_name;
}

// Produces tuples ip:type (V for video, A for audio): 192.168.0.25:V;182.168.0.27:A
char* get_sendto_ip() {
    char* buffer = "";
    for (int i = 0; i < list_length(client_data_cam_list); i++) {
        list_item* item = list_get(client_data_cam_list, i);
        client_data* data = (client_data*)item->data;
        buffer = concat(buffer, data->src_ip);
        buffer = concat(buffer, ":");
        buffer = concat(buffer, "V");
        if (i < list_length(client_data_cam_list) - 1) {
            buffer = concat(buffer, ";");
        }
    }
    if (list_length(client_data_mic_list) > 0) {
        buffer = concat(buffer, ";");
    }
    // TODO: Handle mic path
    for (int i = 0; i < list_length(client_data_mic_list); i++) {
        list_item* item = list_get(client_data_mic_list, i);
        client_data* data = (client_data*)item->data;
        buffer = concat(buffer, data->src_ip);
        buffer = concat(buffer, ":");
        buffer = concat(buffer, "A");
        if (i < list_length(client_data_mic_list) - 1) {
            buffer = concat(buffer, ";");
        }
    }
    cop_debug("[get_sendto_ip] %s.", buffer);
    return buffer;
}

static const char* get_hostname() {
    int MAX_LENGTH = 256;
    char* hostname = malloc(sizeof(char) * (MAX_LENGTH + 1));
    int ret = gethostname(hostname, MAX_LENGTH + 1);
    if (ret != 0) {
        return "n/a";
    }
    return hostname;
}

// Will return: SCAN hostname senderId state width height has_video has_audio
static void tcp_return_scan(int client_socket, const char* senderId, int width, int height, int has_video, int has_audio) {

    const char* buffer = "SCAN ";
    buffer = concat(buffer, get_hostname());
    buffer = concat(buffer, " ");
    buffer = concat(buffer, senderId);
    buffer = concat(buffer, " ");
    buffer = concat(buffer, get_state_str());
    buffer = concat(buffer, " ");
    buffer = concat(buffer, int_to_str(width));
    buffer = concat(buffer, " ");
    buffer = concat(buffer, int_to_str(height));
    buffer = concat(buffer, " ");
    buffer = concat(buffer, int_to_str(has_video));
    buffer = concat(buffer, " ");
    buffer = concat(buffer, int_to_str(has_audio));

    int size = strlen(buffer);

    cop_debug("[tcp_return_scan] Send '%d' bytes to caller.", size);

    int sizeLeftToSend = size;
    
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        
        int result = send(client_socket, buffer, buffSizeToSend, 0);
        if (result < 0) {
            cop_error("[tcp_return_scan] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        buffer = buffer + buffSizeToSend;
    }

    cop_debug("[tcp_return_scan] Finishing: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_scan] Finishing: Close socket.");
    close(client_socket);
}

// Will return: STATUS temperature (in milli degrees, divide by 1000)
static void tcp_return_status(int client_socket, const char* senderId) {

    int temp_in_milli_degrees;
    FILE* temp_file = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (temp_file == NULL) {
        cop_error("[tcp_return_status] Temperature not available.");
    }
    fscanf(temp_file, "%d", &temp_in_milli_degrees);
    fclose(temp_file);

    const char* buffer = "STATUS";
    buffer = concat(buffer, " ");
    buffer = concat(buffer, senderId);
    buffer = concat(buffer, " ");
    buffer = concat(buffer, int_to_str(temp_in_milli_degrees));

    int size = strlen(buffer);

    cop_debug("[tcp_return_status] Send '%d' bytes to caller.", size);

    int sizeLeftToSend = size;
    
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        
        int result = send(client_socket, buffer, buffSizeToSend, 0);
        if (result < 0) {
            cop_error("[tcp_return_status] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        buffer = buffer + buffSizeToSend;
    }

    cop_debug("[tcp_return_status] Finishing: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_status] Finishing: Close socket.");
    close(client_socket);
}

static void tcp_return_download(int client_socket, const char* fileName) {
    FILE* download_file = fopen(fileName, "rb");

    // Calc the size needed
    fseek(download_file, 0, SEEK_END); 
    int size = ftell(download_file);
    fseek(download_file, 0, SEEK_SET);
    // Allocale space on heap
    char* download_buffer = malloc(size);
    memset(download_buffer, '\0', size);

    cop_debug("[tcp_return_download] Read %d bytes from file.", size);

    fread(download_buffer, 1, size, download_file);

    cop_debug("[tcp_return_download] Successfully read binary data.");

    int overall = 0;

    int sizeLeftToSend = size;
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }

        // Do encryption
        // TODO: Assume camera password
        if (encryption_pwd_cam != NULL) {
            size_t pwd_length = strlen(encryption_pwd_cam);
            if (pwd_length > 0) {
                for (int i = 0; i < buffSizeToSend; i++) {
                    download_buffer[i] = download_buffer[i] ^ encryption_pwd_cam[overall % pwd_length];
                    overall++;
                }
            }
        }
        
        int result = send(client_socket, download_buffer, buffSizeToSend, 0);
        if (result < 0) {
            //close(client_socket);
            cop_error("[tcp_return_download] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        download_buffer = download_buffer + buffSizeToSend;
    }

    cop_debug("[tcp_return_download] Finishing download: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_download] Finishing download: Close socket.");
    close(client_socket);
    cop_debug("[tcp_return_download] Finishing download: Close file.");
    fclose(download_file);
}

static void tcp_return_list_files(int client_socket) {

    struct dirent *entry;
    DIR *dir = opendir("./");
    if (dir == NULL) {
        return;
    }

    struct list_item* file_list = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (contains(entry->d_name, "video_") || contains(entry->d_name, "audio_")) {
            FileItem* file_item = malloc(sizeof(FileItem));
            file_item->file_name = strdup(entry->d_name);
            file_list = list_push(file_list, file_item);
            FILE* file = fopen(entry->d_name, "rb");
            fseek(file, 0, SEEK_END);
            file_item->file_size_kb = ftell(file) / 1024;
            fclose(file);
        }
    }
    closedir(dir);

    const char* buffer = "LIST_FILES";

    for (int i = 0; i < list_length(file_list); i++) {
        list_item* item = list_get(file_list, i);
        FileItem* file = (FileItem*)item->data;
        buffer = concat(buffer, " ");
        buffer = concat(buffer, file->file_name);
        buffer = concat(buffer, ";");
        buffer = concat(buffer, int_to_str(file->file_size_kb));
    }

    int size = strlen(buffer);

    cop_debug("[tcp_return_list_files] Send '%d' bytes to caller.", size);

    int sizeLeftToSend = size;
    
    for (int j = 0; j < size; j+=BUFFER_SIZE) {
        
        int buffSizeToSend = BUFFER_SIZE;
        if (sizeLeftToSend < BUFFER_SIZE) {
            buffSizeToSend = sizeLeftToSend;
        }
        
        int result = send(client_socket, buffer, buffSizeToSend, 0);
        if (result < 0) {
            cop_error("[tcp_return_list_files] Send failed: %d.", result);
            break;
        }
        sizeLeftToSend -= BUFFER_SIZE;
        // Set start position of sending data
        buffer = buffer + buffSizeToSend;
    }

    cop_debug("[tcp_return_list_files] Finishing: Shutdown socket.");
    shutdown(client_socket, SHUT_RDWR);
    cop_debug("[tcp_return_list_files] Finishing: Close socket.");
    close(client_socket);
}

int network_receive_tcp(void* arg) {

    system_config* config = (system_config*)arg;

    struct sockaddr_in serv_addr;
    struct sockaddr_in client_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT_LISTEN_COMMAND_TCP);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    receive_tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (receive_tcp_socket < 0) {
        cop_error("[network_receive_tcp] Could not create socket.");
        return STATUS_CODE_NOK;
    }

    // Bind
    if (bind(receive_tcp_socket, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0) {
        cop_error("[network_receive_tcp] Bind failed.");
        // TODO: Remove this
        exit(-1);
        return STATUS_CODE_NOK;
    }

    cop_debug("[network_receive_tcp] Start listening for clients.");

    listen(receive_tcp_socket, 1);

    int c = sizeof(struct sockaddr_in);

    while (true) {

        cop_debug("[network_receive_tcp] Wait for clients: %d.", receive_tcp_socket);

        int client_socket = accept(receive_tcp_socket, (struct sockaddr*) &client_addr, (socklen_t*)&c);

        if (client_socket < 0) {
            cop_error("[network_receive_tcp] Accept failed: %d.", client_socket);
            continue;
        }

        cop_debug("[network_receive_tcp] Accept incoming connection.");

        // Receive data
        int read_size = 0;
        char* buffer = malloc(sizeof(char) * BUFFER_SIZE);
        memset(buffer, '\0', BUFFER_SIZE);

        if ((read_size = recv(client_socket, buffer, BUFFER_SIZE, 0)) > 0) {
            char* string = strdup(buffer);
            if (string == NULL) {
                close(client_socket);
                cop_error("[network_receive_tcp] Receive failed.");
                continue;
            }

            int i = 0;
            char* token;
            char* cmd;
            while ((token = strsep(&buffer, " ")) != NULL) {
                cop_debug("[network_receive_tcp] Token: %s", token);
                if (i == 0) {
                    cmd = token;

                    // LIST_FILES: Returns list of files
                    if (equals(cmd, "LIST_FILES")) {
                        tcp_return_list_files(client_socket);
                        break;
                    }

                    // STATUS: Returns status like temperature
                    if (equals(cmd, "STATUS")) {
                        tcp_return_status(client_socket, config->senderId);
                        break;
                    }

                    // SCAN: Returns device id, width and height
                    if (equals(cmd, "SCAN")) {
                        // Check if client data already exists
                        /*bool exists = false;
                        for (int i = 0; i < list_length(client_data_list); i++) {
                            list_item* item = list_get(client_data_list, i);
                            client_data* data = (client_data*)item->data;
                            exists = equals(data->src_ip, inet_ntoa(client_addr.sin_addr));
                        }
                        if (!exists) {
                            // Add client data
                            client_data* data = malloc(sizeof(client_data));
                            data->src_ip = inet_ntoa(client_addr.sin_addr);
                            cop_debug("[network_receive_tcp] Ip does not exists yet: %s. Add to list.", data->src_ip);
                            list_push(client_data_list, data);
                        }*/
                        // Notify all about change
                        /*for (int i = 0; i < list_length(client_data_list); i++) {
                            list_item* item = list_get(client_data_list, i);
                            client_data* data = (client_data*)item->data;
                            cop_debug("[network_receive_tcp] Notify %s.", data->src_ip);
                        }*/
                        tcp_return_scan(client_socket, config->senderId, config->width, config->height, config->has_video, config->has_audio);
                        break;
                    }

                    i++;
                    continue;
                }
                // DOWNLOAD: Returns single file
                if (equals(cmd, "DOWNLOAD")) {
                    if (i == 1) {
                        tcp_return_download(client_socket, token);
                        i++;
                        break;
                    }
                }
                i++;
            }
        } else {
            close(client_socket);
            cop_error("[network_receive_tcp] Receive failed.");
            continue;
        }
    }

    cop_debug("[network_receive_tcp] Done.");

    return STATUS_CODE_OK;
}
