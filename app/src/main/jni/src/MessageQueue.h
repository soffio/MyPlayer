//
// Created by 马永斌 on 16/6/26.
//

#ifndef MYPLAYER_MESSAGEQUEUE_H
#define MYPLAYER_MESSAGEQUEUE_H

#include <deque>

typedef struct {
    uint8_t messageCode;
    void *p_message;
} Message;


class MessageQueue {
private:
    std::deque<Message> mQueue;

public:
    MessageQueue();

    void sendMessage(Message msg);

    bool popMessage(Message& msg);
};


#endif //MYPLAYER_MESSAGEQUEUE_H
