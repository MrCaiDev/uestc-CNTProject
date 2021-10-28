/**
 * @file    net.cpp
 * @author  蔡与望
 * @brief   主机网络层。
 */
#include <iostream>
#include <winsock2.h>
#include "../include/param.h"
#include "../include/coding.cpp"
#include "../include/frame.cpp"
#include "../include/socket.cpp"
using namespace std;

int main(int argc, char *argv[]) {
    cout << "-------------NET-------------" << endl;
    // 确定端口，推荐通过命令行传参。
    unsigned short appPort = 0, netPort = 0, phyPort = 0;
    if (argc == 4) {
        appPort = atoi(argv[1]);
        netPort = atoi(argv[2]);
        phyPort = atoi(argv[3]);
        cout << "APP Port: " << appPort << endl
             << "NET Port: " << netPort << endl
             << "PHY Port: " << phyPort << endl;
    } else {
        // 手动传参。
        cout << "APP Port: ";
        cin >> appPort;
        cout << "NET Port: ";
        cin >> netPort;
        cout << "PHY Port: ";
        cin >> phyPort;
    }
    // 初始化变量。
    unsigned short srcPort = BROADCAST_PORT, seq = 0, dstPort = 0;
    int mode = 0;
    char buffer[MAX_BUFFER_SIZE];
    string message = "";
    string recvMessage = "";
    string tempMessage = "";
    int sendTotal = 0;
    int recvTotal = 0;
    int recvBytes = 0;
    // 初始化网络库与套接字。
    WSADATA wsaData = initWSA();
    NetSocket sock(netPort);
    sock.bindApp(appPort);
    sock.bindPhy(phyPort);
    cout << "---------Initialized---------" << endl;

    while (true) {
        /* ---------------------上层通知当前模式。---------------------- */
        sock.recvFromApp(buffer, USER_TIMEOUT);
        mode = atoi(buffer);

        /* -------------------------接收模式。------------------------- */
        if (mode == RECV) {
            recvMessage.clear();
            // 逐帧接收。只有发送了ACK，循环才有步进。
            for (int frame = 0; frame <= recvTotal;) {
                if (frame == 0) {
                    // 起始帧由于需要等发送端用户输入，所以可以等得久一点。
                    recvBytes = sock.recvFromPhy(buffer, USER_TIMEOUT);
                } else {
                    // 其它帧不能等太久。
                    recvBytes = sock.recvFromPhy(buffer, RECV_TIMEOUT);
                }
                // 如果超时没收到消息，回复NAK。
                if (recvBytes <= 0) {
                    cout << "[Frame " << seq + 1 << "] Timeout." << endl;
                    Frame nak(appPort, seq + 1, encode(NAK), srcPort);
                    sock.sendToPhy(nak.stringify());
                    continue;
                }
                // 如果收到了，就先解封。
                Frame recvFrame(buffer);
                // 先检查目标端口：
                // 如果发来的帧不是给自己的，既不回复也不接收。
                if (appPort != recvFrame.getDstPort()) {
                    continue;
                }
                // 再检查序号：
                // 如果发来的帧序号重复了，回复ACK，但不接收这帧。
                // 这种情况说明，这帧是发送端收到了不明回复后重传的。其实接收端已经ACK过这帧了。
                // 再传一次ACK，让发送端放心。
                if (seq == recvFrame.getSeq()) {
                    cout << "[Frame " << seq << "] Repeated." << endl;
                    Frame ack(appPort, seq, encode(ACK), srcPort);
                    sock.sendToPhy(ack.stringify());
                    continue;
                }
                // 最后检查校验和：
                // 通过则回复ACK，失败则回复NAK。
                if (recvFrame.isVerified()) {
                    // 更新序号。
                    seq = recvFrame.getSeq();
                    if (frame == 0) {
                        // 如果是起始帧，就据此更新循环次数。
                        recvTotal = atoi(decode(recvFrame.getData()).c_str());
                        srcPort = recvFrame.getSrcPort();
                        cout << "[Frame " << seq << "] Receiving " << recvTotal
                             << " frames." << endl;
                    } else {
                        // 如果不是起始帧，就拼接消息。
                        recvMessage += recvFrame.getData();
                        cout << "[Frame " << seq << "] Verified." << endl;
                    }
                    // 不管是不是起始帧，都要回复ACK。
                    Frame ack(appPort, seq, encode(ACK), srcPort);
                    sock.sendToPhy(ack.stringify());
                    // 可以接收下一帧了。
                    ++frame;
                } else {
                    // 不更新序号，不拼接消息。直接回复NAK。
                    cout << "[Frame " << seq + 1 << "] Invalid." << endl;
                    Frame nak(appPort, seq + 1, encode(NAK), srcPort);
                    sock.sendToPhy(nak.stringify());
                }
            }
            // 把拼接完的消息交给应用层。
            sock.sendToApp(recvMessage);

            /* ---------------------发送模式。------------------------- */
        } else if (mode == UNICAST || mode == BROADCAST) {
            // 确定目标端口。
            if (mode == UNICAST) {
                sock.recvFromApp(buffer, USER_TIMEOUT);
                dstPort = atoi(buffer);
                cout << "Unicasting to port " << dstPort << "." << endl;
            } else {
                dstPort = BROADCAST_PORT;
            }
            // 确定要发的消息。
            sock.recvFromApp(buffer, USER_TIMEOUT);
            message = buffer;
            // 计算消息要分多少帧，然后先把这些帧全打包好。
            sendTotal = Frame::calcTotal(message.length());
            Frame *packages = new Frame[sendTotal + 1];
            // 第0帧是特殊帧，用于通知对方要发多少帧。
            seq = (seq + 1) % 256;
            Frame request(appPort, seq, encode(to_string(sendTotal)), dstPort);
            packages[0] = request;
            // 其它帧瓜分应用层消息。
            for (int frame = 1; frame <= sendTotal; frame++) {
                if (frame == sendTotal) {
                    // 如果是最后一帧，就直接取走所有剩余消息。
                    tempMessage = message;
                } else {
                    // 如果不是最后一帧，就只取走剩余消息的一部分。
                    tempMessage = message.substr(0, DATA_LEN);
                    message = message.substr(DATA_LEN);
                }
                // 把取出来的消息封装进帧。
                seq = (seq + 1) % 256;
                Frame readyFrame(appPort, seq, tempMessage, dstPort);
                packages[frame] = readyFrame;
            }
            // 逐帧发送。只有收到了ACK，循环才有步进。
            for (int frame = 0; frame <= sendTotal;) {
                if (frame == 0) {
                    // 如果是起始帧，就显示发送总帧数。
                    cout << "[Frame " << packages[frame].getSeq()
                         << "] Sending " << sendTotal << " frames." << endl;
                } else {
                    // 如果是其它帧，就只显示发送了。
                    cout << "[Frame " << packages[frame].getSeq() << "] Sent."
                         << endl;
                }
                // 发送给对方。
                sock.sendToPhy(packages[frame].stringify());
                // 接收对方的回复。
                recvBytes = sock.recvFromPhy(buffer, RECV_TIMEOUT);
                // 如果没收到回复，重传。
                if (recvBytes <= 0) {
                    cout << "[Frame " << packages[frame].getSeq()
                         << "] Timeout." << endl;
                    continue;
                }
                // 如果收到了，就解封并读取回复。
                Frame response(buffer);
                string responseMessage = decode(response.getData());
                if (responseMessage == ACK) {
                    // 如果是ACK，就打印成功信息。
                    cout << "[Frame " << packages[frame].getSeq() << "] ACK."
                         << endl;
                    // 可以发送下一帧了。
                    ++frame;
                } else if (responseMessage == NAK) {
                    // 如果是NAK，就报错并重传。
                    cout << "[Frame " << packages[frame].getSeq() << "] NAK."
                         << endl;
                } else {
                    // 如果是其他信息，说明对面的回复在传的时候也出错了，还是要重传这一帧。
                    // ! 这里有一个潜在的漏洞：
                    // 如果接收端对最后一帧的ACK传错了，那么接收端已经停止接收，但发送端仍会继续重传。
                    // 这会导致发送端无法终止。考虑引入keepalive机制。
                    cout << "[Frame " << packages[frame].getSeq()
                         << "] Unknown response." << endl;
                }
            }
            // 全部发完，封装的帧可以丢弃。
            delete[] packages;

            /* ---------------------退出程序。------------------------- */
        } else if (mode == QUIT) {
            break;
        }
        // 结束一轮后，画一条分割线。
        cout << "-----------------------------" << endl;
    }
    quit();
}
