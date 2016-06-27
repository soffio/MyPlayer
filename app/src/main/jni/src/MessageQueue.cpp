//
// Created by 马永斌 on 16/6/26.
//

#include "MessageQueue.h"

MessageQueue::MessageQueue() {

}


void MessageQueue::sendMessage(Message msg) {
    mQueue.push_back(msg);
}


bool MessageQueue::popMessage(Message &msg) {
    if (mQueue.empty())
        return false;
    Message frontMessage = mQueue.front();
    mQueue.pop_front();
    msg.messageCode = frontMessage.messageCode;
    msg.p_message = frontMessage.p_message;
}
