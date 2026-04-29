FROM ubuntu:24.04 AS build
ARG MODEL=Release
#rtsp,http
EXPOSE 554/tcp
EXPOSE 8089/tcp

# ADD sources.list /etc/apt/sources.list

RUN apt-get update && \
         DEBIAN_FRONTEND="noninteractive" \
         apt-get install -y --no-install-recommends \
         build-essential \
         cmake \
         git \
         curl \
         vim \
         wget \
         ca-certificates \
         tzdata \
         libssl-dev \
         gcc \
         g++ \
         python3-dev \
         gdb && \
         apt-get autoremove -y && \
         apt-get clean -y && \
         rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/media
COPY . /opt/media/ZLMediaKit
WORKDIR /opt/media/ZLMediaKit

# 3rdpart init
WORKDIR /opt/media/ZLMediaKit/3rdpart
RUN wget https://github.com/cisco/libsrtp/archive/v2.3.0.tar.gz -O libsrtp-2.3.0.tar.gz && \
    tar xfv libsrtp-2.3.0.tar.gz && \
    mv libsrtp-2.3.0 libsrtp && \
    cd libsrtp && CFLAGS="-fcommon" ./configure --enable-openssl && make -j $(nproc) && make install
#RUN git submodule update --init --recursive && \

RUN mkdir -p build release/linux/${MODEL}/

WORKDIR /opt/media/ZLMediaKit/build
RUN cmake -DENABLE_PYTHON=true -DCMAKE_BUILD_TYPE=${MODEL} -DENABLE_WEBRTC=true -DENABLE_FFMPEG=true -DENABLE_TESTS=false -DENABLE_API=false .. && \
    make -j $(nproc)

FROM ubuntu:24.04 AS runtime
ARG MODEL=Release

# ADD sources.list /etc/apt/sources.list

RUN apt-get update && \
         DEBIAN_FRONTEND="noninteractive" \
         apt-get install -y --no-install-recommends \
         ca-certificates \
         tzdata \
        openssl && \
         apt-get autoremove -y && \
         apt-get clean -y && \
    rm -rf /var/lib/apt/lists/*

ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
        && echo $TZ > /etc/timezone && \
    mkdir -p /opt/media/bin/www /opt/media/conf /opt/media/bin/log

WORKDIR /opt/media/bin/
COPY --from=build /opt/media/ZLMediaKit/release/linux/${MODEL}/MediaServer /opt/media/ZLMediaKit/default.pem /opt/media/bin/
COPY --from=build /opt/media/ZLMediaKit/release/linux/${MODEL}/config.ini /opt/media/conf/
COPY --from=build /opt/media/ZLMediaKit/www/ /opt/media/bin/www/

ENV PATH /opt/media/bin:$PATH
CMD ["./MediaServer","-s", "default.pem", "-c", "../conf/config.ini", "--log-dir", "/opt/media/bin/log", "-l","0"]
