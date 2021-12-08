#include <stdio.h>

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
    bzero(servaddr, sizeof(servaddr));
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
    ret = listen(LISTEN_SIZE);
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
    sockaddr_in clitaddr;
    bzero(clitaddr, sizeof(clitaddr));
    socklen_t clitlen = sizeof(clitaddr);

    //阻塞监听建立连接，但因为是先通过epoll事件监听到有读事件发生才调用accept，所以不会在此发生阻塞
    int cfd = accept(lfd, (struct sockaddr *)clitaddr, &clitlen);
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
    ev.event = EPOLLIN | EPOLLET;
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
    int n;
    while ((i < size-1) && (c != '\n')) {
        n = recv(cfd, &c, 1, 0);
        if (n > 0) {
            if (c == '\r') {
                //MSG_PEEK 偷看，模拟读取，读取到数据但不会使缓冲区被读取的数据被清除
                n = recv(cfd, &c, 1, MSG_PEEK);
                if ((n > 0) && (c != '\n')) {
                    recv(cfd, &c, 1, 0);
                } else {
                    c = '\n';
                }
            }
            buf[i] = c;
            i++;
        } else {
            c = '\n';
        }
    }//while
    buf[i] = '\n';

    if (n == -1) 
        i = n;

    return i;
}
void do_read(int cfd, int epfd)
{
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
            if (!(all_events[i].event & EPOLLIN)) //若不是读事件，则直接跳过
                continue;
            if (all_event[i].data.fd == lfd)
                do_accept(all_event[i].data.fd, epfd);
            else 
                do_read(all_event[i].data.fd, epfd);
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
