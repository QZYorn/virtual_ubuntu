#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define EPOLL_SIZE 20
#define LISTEN_SIZE 20

int init_listen_fd(int port, int epfd)
{
    int lfd;
    int ret;
    //创建监听套接字
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("lfd socket error");
        exit(1);
    }

    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    //设置端口复用
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    //给lfd绑定地址结构
    ret = bind(lfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    if (ret == -1) {
        perror("bind error");
        exit(1);
    }

    //设置监听上限
    ret = listen(lfd, LISTEN_SIZE);
    if (ret == -1) {
        perror("listen error");
        exit(1);
    }

    //lfd挂上epoll红黑树监听
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("lfd epoll_ctl error");
        exit(1);
    }

    return lfd;
}

void do_accept(int lfd, int epfd)
{
    struct sockaddr_in clitaddr;
    bzero(&clitaddr, sizeof(clitaddr));
    socklen_t clitlen = sizeof(clitaddr);

    //阻塞监听建立连接，但因为是先通过epoll事件监听到有读事件发生才调用accept，所以不会在此发生阻塞
    int cfd = accept(lfd, (struct sockaddr *)&clitaddr, &clitlen);
    if (cfd == -1) {
        perror("accept error");
        exit(1);
    }

    //打印端口号和IP地址
    char clit_IP[64] = {0};
    printf("New client IP:%s, port: %d, cfd = %d\n", 
           inet_ntop(AF_INET, &clitaddr.sin_addr.s_addr, clit_IP, sizeof(clit_IP)), 
           ntohs(clitaddr.sin_port), 
           cfd);

    //设置非阻塞
    int flag = fcntl(cfd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);

    //设置ET模式，并挂上树
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = cfd;
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("cfd epoll_ctl error");
        exit(1);
    }
}

int get_line(int cfd, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n = 0;
    while ((i < size-1) && (c != '\n')) {
        n = recv(cfd, &c, 1, 0);
        if (n > 0) {
            if (c == '\r') {
                //MSG_PEEK 偷看，模拟读取，读取到数据但不会使缓冲区被读取的数据被清除
                n = recv(cfd, &c, 1, MSG_PEEK);

                if (n > 0)
                    recv(cfd, &c, 1, 0);
                else
                    c = '\n';
            }
            buf[i] = c;
            i++;
        } else {
            c = '\n';
        }
    }//while
    buf[i] = '\0';

    if (n == -1) 
        i = n;

    return i;
}

//停止监听cfd
void disconnect(int cfd, int epfd)
{
    int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
    if (ret == -1) {
        perror("epoll_ctl DEL error");
        exit(1);
    }
    close(cfd);
}

//回发错误界面
void send_error(int cfd, int status, char *title, char *text)
{
    char buf[4096] = {0};

    sprintf(buf, "%s %d %s\r\n", "HTTP/1.1", status, title);
    sprintf(buf + strlen(buf), "Content-Type:%s\r\n", "text/html");
    sprintf(buf + strlen(buf), "Content-Length:%d\r\n", -1);
    sprintf(buf + strlen(buf), "Contection: close\r\n");

    send(cfd, buf, strlen(buf), 0);
    send(cfd, "\r\n", 2, 0);

    memset(buf, 0, sizeof(buf));

    sprintf(buf, "<html><head><title>%d %s</title></head>\n", status, title);
    sprintf(buf + strlen(buf), "<body bgcolor=\"#cc99cc\"><h2 align=\"center\">%d %s</h4>\n", status, title);
    sprintf(buf + strlen(buf), "%s\n", text);
    sprintf(buf + strlen(buf), "<hr>\n</body>\n</html>\n");

    send(cfd, buf, strlen(buf), 0);

    return;
}

//回发应答协议栈首部
//客户端的fd, 错误号, 错误描述, 回发文件类型, 文件长度
void send_respond(int cfd, int no, char *disp, char *type, int len)
{
    char buf[1024] = {0};
    sprintf(buf, "HTTP/1.1 %d %s\r\n", no, disp);
    sprintf(buf + strlen(buf), "Content-Type:%s\r\n", type);
    sprintf(buf + strlen(buf), "Content-Length:%d\r\n", len);

    //0表示默认行为, 等同write
    send(cfd, buf, strlen(buf), 0);
    send(cfd, "\r\n", 2, 0);
}

//回发文件内容
void send_file(int cfd, const char *file)
{
    char buf[4096] = {0};
    int fd = open(file, O_RDONLY);
    if (fd == -1) {
        perror("open file error");
        exit(1);
    }

    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        int ret = send(cfd, buf, n, 0);
        if (ret == -1) {
            if (errno == EAGAIN)
                continue;
            else if (errno == EINTR)
                continue;
            else if (errno == EWOULDBLOCK)
                continue;
            else {
                perror("send file error");
                exit(1);
            }
        }
        if (ret < 4096)
            printf("---------  send ret = %d   ----------\n", ret);
    }//while

    close(fd);
}

//16进制数转化为10进制数
int hexit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

//编码 汉字转URL
void encode_str(char *to, int tosize, const char *from)
{
    int tolen;

    for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) {
        //是字母或数字或指定符号
        if (isalnum(*from) || strchr("/_.-~", *from) != (char*)0) {
            *to = *from;
            ++to;
            ++tolen;
        } else {
            //写入例子%B5
            sprintf(to, "%%%02x", (int) *from & 0xff);
            to += 3;
            tolen += 3;
        }
    }
    *to = '\0';
}

//解码 %XX 转 汉字
void decode_str(char *to, char *from)
{
    for ( ; *from != '\0'; ++to, ++from) {
        //isxdigit判断是否16进制数
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            *to = hexit(from[1])*16 + hexit(from[2]);
            from += 2;
        } else {
            *to = *from;
        }
    }
    *to = '\0';
}

void send_dir(int cfd, const char *file)
{
    int i, ret;

    //拼一个html页面<table></table>
    char buf[4096] = {0};

    sprintf(buf, "<html><head><title>目录名：%s</title></head", file);
    sprintf(buf + strlen(buf), "<body><h1>当前目录：%s</h1><table>", file);

    char enstr[1024] = {0};
    char path[1024] = {0};

    //目录项二级指针
    struct dirent **dent;
    int num = scandir(file, &dent, NULL, alphasort);

    //遍历
    for (i = 0; i < num; ++i) {
        printf("========== %d\n", i);
        char *name = dent[i]->d_name;

        //拼接文件的完整路径
        sprintf(path, "%s/%s", file, name);
        struct stat st;
        stat(path, &st);

        encode_str(enstr, sizeof(enstr), name); 
        //strcpy(enstr, name);

        if (S_ISREG(st.st_mode)) {          //如果是文件
            sprintf(buf + strlen(buf), 
                    "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", 
                    enstr, name, (long)st.st_size);
        } else if (S_ISDIR(st.st_mode)) {   //如果是目录
            sprintf(buf + strlen(buf), 
                    "<tr><td><a href=\"%s/\">%s/</a></td><td>%ld</td></tr>",
                    enstr, name, (long)st.st_size);
        }
        ret = send(cfd, buf, strlen(buf), 0);
        if (ret == -1) {
            if (errno == EAGAIN) {
                perror("send errpr");
                continue;
            } else if (errno == EINTR) {
                perror("send error");
                continue;
            } else {
                perror("send error");
                exit(1);
            }
        }
        bzero(buf, sizeof(buf));
    }//for
    sprintf(buf + strlen(buf), "</table></body></html>");
    send(cfd, buf, strlen(buf), 0);
    printf("dir message send OK!!!!!\n");
    /*
       DIR *dir = opendir(file);
       if (dir == NULL) {
       perror("opendir error");
       exit(1);
       }

       strtuct dirent *dent = readdir(dir);
       if (dent == NULL) {
       perror("readdir error");
       exit(1);
       }
       dent->d_ino;
       dent->d_name;

       close(dir);
       */   
}

//根据文件名获取文件类型并返回对应的http应答头Type组成部分
const char *get_file_type(const char *name)
{
    char *dot;
    //自右向左查找‘.’字符，如查找不到则返回NULL
    dot = strrchr(name, '.');
    //"Content-Type: audio/mpeg; charset=iso-8859-1"
    if (dot == NULL)
        return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jepg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "audio/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}

//处理http请求,判断文件是否存在,回发
void http_request(int cfd, const char *file)
{
    //如果没有指定访问的资源，则使用默认目录作为访问对象
    char *path;
    if (strcmp(file, "/") == 0)
        path = "./";
    else
        path = (char *)file + 1;

    //获取文件状态并判断文件是否存在
    struct stat sbuf;
    int ret = stat(path, &sbuf);   
    if (ret != 0) {
        //回发浏览器 404错误信息
        printf("404:%s\n", path);
        send_error(cfd, 404, "Not Found", "No such file or direntry");
    }

    //判断是文件还是目录
    if (S_ISDIR(sbuf.st_mode)) {                //若是一个目录
        printf("--------------it is a dir\n");
        send_respond(cfd, 200, "OK", (char *)get_file_type(".html"), -1);
        send_dir(cfd, path);
    } else if (S_ISREG(sbuf.st_mode)) {         //若是一个普通文件
        printf("--------------it is a file\n");
        //回发http协议应答
        //send_respond(cfd, 200, "OK", "Content-Type: text/plain; charset=iso-8859-1", sbuf.st_size);
        send_respond(cfd, 200, "OK", (char *)get_file_type(path), sbuf.st_size);
        //回发文件内容
        send_file(cfd, path);
    }
}

void do_read(int cfd, int epfd)
{
    char line[1024]; 
    int len = get_line(cfd, line, sizeof(line));
    if (len == 0) {
        printf("服务器，检测到客户端关闭...\n");
        disconnect(cfd, epfd);
    } else {
        char method[16], path[256], pathbf[256], protocol[16];
        sscanf(line, "%[^ ] %[^ ] %[^ ]", method, pathbf, protocol);
        decode_str(path, pathbf);
        printf("method = %s, path == %s, protocol = %s\n", method, path, protocol);

        char buf[1024] = {0};

        while (1) {
            len = get_line(cfd, buf, sizeof(buf));
            if (len == '\n')
                break;
            else if (len == -1)
                break;
            printf("\t%s\n", buf);
            usleep(100000);
        }
        //case无视大小写
        if (strncasecmp(method, "GET", 3) == 0) {
            //path == /word.txt, path + 1 == word.txt
            http_request(cfd, path);
            disconnect(cfd, epfd);
        }

    }
}

void epoll_run(int port)
{
    int epsize;
    struct epoll_event all_events[EPOLL_SIZE];

    //创建epoll树根
    int epfd = epoll_create(EPOLL_SIZE);
    if (epfd == -1) {
        perror("epoll_create error");
        exit(1);
    }

    //初始化，创建lfd并添加进epoll树
    int lfd = init_listen_fd(port, epfd);
    while (1) {
        epsize = epoll_wait(epfd, all_events, EPOLL_SIZE, -1);
        if (epsize == -1) {
            perror("epoll_wait error");
            exit(1);
        }
        for (int i = 0; i < epsize; i++) {
            if (!(all_events[i].events & EPOLLIN)) //若不是读事件，则直接跳过
                continue;
            if (all_events[i].data.fd == lfd)
                do_accept(all_events[i].data.fd, epfd);
            else 
                do_read(all_events[i].data.fd, epfd);
        }
    }//while (1)
    close(lfd);
    close(epfd);
}

int main(int argc, char *argv[])
{
    //命令行参数获取 端口和 server提供的目录
    if (argc < 3) {
        printf("./a.out port path\n");
    }

    //获取用户输入的端口
    int port = atoi(argv[1]);

    //改变进程工作目录
    int ret = chdir(argv[2]);
    if (ret != 0) {
        perror("chdir error");
        exit(1);
    }

    //启动epoll监听
    epoll_run(port);

    return 0;
}
