//
// Created by 马永斌 on 16/5/24.
//

#ifndef MYPLAYER_FFPLAYER_H
#define MYPLAYER_FFPLAYER_H

#include <stdint.h>
#include <string>

namespace ffplayer {


    class FFPlayer {
    public:
        FFPlayer();

        ~FFPlayer();

        void start();

        void pause();

        void setDataSource(const char *path);

        void prepare();

        void release();

        void seekTo(int64_t seekTimeUs);

        void setSurface();

        void getDuration(int64_t *timeUs);

    private:
        std::string mPath;

    };
}
#endif //MYPLAYER_FFPLAYER_H
