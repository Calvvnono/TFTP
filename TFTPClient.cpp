#include <iostream>
#include <winsock2.h>
#include <string.h>
#include <ctime>
#define LIMIT_T 30
#define BUFLEN 1024
const char* MODE[2]={"netascii","octet"};
int blks_sent;
int blks_resent;

class Msg{
private:
    SYSTEMTIME nowtime;
    double speed;
    char resultMsg[64];
    char filename[32];
    char mode[32];
    bool result;
public:
    void success(SYSTEMTIME t,double s,const char* f,const char* m){
        nowtime=t;
        speed=s;
        strcpy(filename,f);
        strcpy(mode,m);
        result=true;
    }
    void fail(SYSTEMTIME t,const char* msg,const char* f,const char* m){
        nowtime=t;
        strcpy(resultMsg,msg);
        strcpy(filename,f);
        strcpy(mode,m);
        result=false;
    }
    void print(FILE* fp){
        char buf[BUFLEN];
        int len  = sprintf(buf,"%4d/%02d/%02d %02d:%02d:%02d.%03d\n",nowtime.wYear,nowtime.wMonth,nowtime.wDay,nowtime.wHour,nowtime.wMinute, nowtime.wSecond,nowtime.wMilliseconds);  
        switch(result){
            case 0:
                sprintf(buf + len,"fail\tfilename:%s\tmode:%s\t%s\n\n\n",filename,mode,resultMsg);
                break;
            case 1:
                sprintf(buf + len,"success\tfilename:%s\tmode:%s\tspeed:%lfKB/s\t%d blocks sent in total, %d blocks resent\n\n\n",filename,mode,speed,blks_sent,blks_resent);
                break;
        }
        printf("%s",buf);
        fwrite(buf,1,strlen(buf),fp);
        fflush(fp);
    }
}msg;

bool WriteInfo(SOCKET clientSock,int port,const char* IP,int mode,const char* filename){
    SYSTEMTIME T;
    
    unsigned long Opt = 1;
    ioctlsocket(clientSock, FIONBIO, &Opt);
    
    clock_t start,end;
    start = clock();
    
    FILE *fp;
    if(mode==0)fp=fopen(filename,"r");
    else fp=fopen(filename,"rb");
    
    if(fp==NULL){
        GetLocalTime(&T);
        msg.fail(T,"File not found",filename,MODE[mode]);
        return 0;
    }
    
    int nrc,len,filesz=0;
    sockaddr_in srvAddr,tempsrvAddr;int sz=sizeof(srvAddr);
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(port);
    srvAddr.sin_addr.S_un.S_addr = inet_addr(IP);
    
    char sendBuf[BUFLEN],recv[BUFLEN];
    memset(sendBuf,0,BUFLEN);memset(recv,0,BUFLEN);
    
    auto RECV = ([&](int expnum){
            struct timeval tv;
            fd_set readfds;
            for(int i = 0; i < 20; i++){
                FD_ZERO(&readfds);
                FD_SET(clientSock, &readfds);
                tv.tv_sec = LIMIT_T;

                select(clientSock, &readfds, NULL, NULL, &tv);
                if(FD_ISSET(clientSock, &readfds)){
                    if ((nrc = recvfrom(clientSock, recv, BUFLEN, 0, (struct sockaddr*)&tempsrvAddr, &sz)) > 0){
                        if(((unsigned char)recv[2] << 8) + (unsigned char)recv[3]==expnum)
                            return 1;
                    }
                }
                sendto(clientSock, sendBuf, len, 0, (struct sockaddr*)&srvAddr, sz);
                blks_resent ++;
            }
            return 0;
        }
    );

    sprintf(sendBuf, "%c%c%s%c%s%c", 0, 2, filename, 0, MODE[mode], 0);
    len = 4 + strlen(filename) + strlen(MODE[mode]);
    nrc = sendto(clientSock, sendBuf, len, 0, (sockaddr*)&srvAddr, sz);
    if(nrc == SOCKET_ERROR){
        GetLocalTime(&T);
        msg.fail(T,"send WRQ fail",filename,MODE[mode]);
        return 0;
    }
    bool ret = RECV(0);
    if(!ret){
        GetLocalTime(&T);
        msg.fail(T,"recv time out",filename,MODE[mode]);
        return 0;
    }
    if(recv[1]==5){
        GetLocalTime(&T);
        int errcode = ((unsigned char)recv[2] << 8) + (unsigned char)recv[3];
        char buf[BUFLEN];
        sprintf(buf,"error code:%d error message:%s",errcode,recv+4);
        msg.fail(T,buf,filename,MODE[mode]);
        return 0;
    }

    srvAddr = tempsrvAddr;
    while(1){
        int seq = ((unsigned char)recv[2] << 8) + (unsigned char)recv[3] + 1;
        memset(sendBuf,0,BUFLEN);
        seq = (seq==65536?0:seq);
        sendBuf[0] = 0;sendBuf[1] = 3;
        sendBuf[2] = (seq >> 8);sendBuf[3] = seq & 0xff;
        len = fread(sendBuf+4, 1, 512, fp) + 4;
        filesz += len - 4;
        
        nrc = sendto(clientSock, sendBuf, len, 0, (sockaddr*)&srvAddr, sz);
        blks_sent ++;
        bool ret = RECV(seq);
        if(!ret){
            GetLocalTime(&T);
            msg.fail(T,"recv time out",filename,MODE[mode]);
            return 0;
        }
        if(recv[1]==5){
            GetLocalTime(&T);
            int errcode = ((unsigned char)recv[2] << 8) + (unsigned char)recv[3];
            char buf[BUFLEN];
            sprintf(buf,"error code:%d error message:%s",errcode,recv+4);
            msg.fail(T,buf,filename,MODE[mode]);
            return 0;
        }

        if(len<516){
            break;
        }
    }
    end = clock();
    GetLocalTime(&T);
    msg.success(T, filesz/((double)(end-start) / CLK_TCK)/1024, filename, MODE[mode]);
    return 1;
}

bool ReadInfo(SOCKET clientSock,int port,const char* IP,int mode,const char* filename){
    SYSTEMTIME T;
    
    unsigned long Opt = 1;
    ioctlsocket(clientSock, FIONBIO, &Opt);

    clock_t start,end;
    start = clock();

    FILE *fp;
    if(mode==0)fp=fopen(filename,"w");
    else fp=fopen(filename,"wb");

    int nrc,len,filesz=0;
    sockaddr_in srvAddr,tempsrvAddr;int sz=sizeof(srvAddr);
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(port);
    srvAddr.sin_addr.S_un.S_addr = inet_addr(IP);

    char sendBuf[BUFLEN],recv[BUFLEN];
    memset(sendBuf,0,BUFLEN);memset(recv,0,BUFLEN);

    auto RECV = ([&](int expnum){
            struct timeval tv;
            fd_set readfds;
            for(int i = 0; i < 20; i++){
                FD_ZERO(&readfds);
                FD_SET(clientSock, &readfds);
                tv.tv_sec = LIMIT_T;

                select(clientSock, &readfds, NULL, NULL, &tv);
                if(FD_ISSET(clientSock, &readfds)){
                    if ((nrc = recvfrom(clientSock, recv, BUFLEN, 0, (struct sockaddr*)&tempsrvAddr, &sz)) > 0){
                        if(((unsigned char)recv[2] << 8) + (unsigned char)recv[3]==expnum)
                            return 1;
                    }
                }
                sendto(clientSock, sendBuf, len, 0, (struct sockaddr*)&srvAddr, sz);
                blks_resent ++;
            }
            return 0;
        }
    );

    sprintf(sendBuf, "%c%c%s%c%s%c", 0, 1, filename, 0, MODE[mode], 0);
    len = 4 + strlen(filename) + strlen(MODE[mode]);
    nrc = sendto(clientSock, sendBuf, len, 0, (sockaddr*)&srvAddr, sz);
    if(nrc == SOCKET_ERROR){
        GetLocalTime(&T);
        msg.fail(T,"send RRQ fail",filename,MODE[mode]);
        return 0;
    }
    bool ret = RECV(1);
    if(!ret){
        GetLocalTime(&T);
        msg.fail(T,"recv time out",filename,MODE[mode]);
        return 0;
    }
    if(recv[1]==5){
        GetLocalTime(&T);
        int errcode = ((unsigned char)recv[2] << 8) + (unsigned char)recv[3];
        char buf[BUFLEN];
        sprintf(buf,"error code:%d error message:%s",errcode,recv+4);
        msg.fail(T,buf,filename,MODE[mode]);
        return 0;
    }
    srvAddr = tempsrvAddr;
    while(1){
        filesz += nrc - 4;
        int seq = ((unsigned char)recv[2] << 8) + (unsigned char)recv[3];
        memcpy(sendBuf,recv,4);
        if(nrc<516){
            fwrite(recv+4,1,nrc-4,fp);
            fflush(fp);
            sendBuf[1] = 4;
            len = 4;
            sendto(clientSock, sendBuf, 4, 0, (sockaddr*)&srvAddr, sizeof(srvAddr));
            blks_sent ++;
            break;
        }
        else{
            fwrite(recv+4,1,512,fp);
            fflush(fp);
            sendBuf[1] = 4;
            len = 4;
            sendto(clientSock, sendBuf, 4, 0, (sockaddr*)&srvAddr, sizeof(srvAddr));
            blks_sent ++;
            bool ret = RECV(seq + 1);
            if(!ret){
                GetLocalTime(&T);
                msg.fail(T,"recv time out",filename,MODE[mode]);
                return 0;
            }
            if(recv[1]==5){
                GetLocalTime(&T);
                int errcode = ((unsigned char)recv[2] << 8) + (unsigned char)recv[3];
                char buf[BUFLEN];
                sprintf(buf,"error code:%d error message:%s",errcode,recv+4);
                msg.fail(T,buf,filename,MODE[mode]);
                return 0;
            }
        }
    }
    end = clock();
    GetLocalTime(&T);
    msg.success(T, filesz/((double)(end-start) / CLK_TCK)/1024, filename, MODE[mode]);
    return 1;
}

int main(int argc, char* argv[]){
    FILE* logfp;
    logfp = fopen("log.txt","w");
    fwrite("log\n",1,4,logfp);

    WSADATA wsaData;
    int nRC;
    SOCKET clientSock;
    u_long uNonBlock;
    nRC = WSAStartup(0x0101, &wsaData);
    if(nRC){
        printf("Client initialize winsock error!\n");
        return 0;
    }
    printf("Client's winsock initialized!\n");
    clientSock = socket(AF_INET,SOCK_DGRAM,0);
    if(clientSock == INVALID_SOCKET){
        printf("Client create socket error!\n");
        WSACleanup();
        return 0;
    }
    int opt;
    bool flag=0;
    char desip[20],filename[20];
    printf("Client socket create OK!\n");
    printf("Please enter server IP address:");
    scanf("%s",desip);
    printf("\nPlease enter commands according to the following format:\n1 filename //upload file in netascii mode\n2 filename //upload file in octet mode\n3 filename //download file in netascii mode\n4 filename //download file in octet mode\n-1 //close client\n\n");
    while(1){
    	blks_sent = 0;
    	blks_resent = 0;
        scanf("%d",&opt);
        switch(opt){
            case 1:
                scanf("%s",&filename);
                WriteInfo(clientSock,69,desip,0,filename);
                msg.print(logfp);
                break;
            case 2:
                scanf("%s",&filename);
                WriteInfo(clientSock,69,desip,1,filename);
                msg.print(logfp);
                break;
            case 3:
                scanf("%s",&filename);
                ReadInfo(clientSock,69,desip,0,filename);
                msg.print(logfp);
                break;
            case 4:
                scanf("%s",&filename);
                ReadInfo(clientSock,69,desip,1,filename);
                msg.print(logfp);
                break;
            case -1:
                flag=1;
                break;
            default:
                break;
        }
        if(flag){
            break;
        }
    }
    closesocket(clientSock);
    WSACleanup();
}
