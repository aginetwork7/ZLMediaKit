# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04 AS build
ARG MODEL=Release
#rtsp,http,https
EXPOSE 554/tcp
EXPOSE 8089/tcp
EXPOSE 8449/tcp
# ADD sources.list /etc/apt/sources.list

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
         --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
         apt-get update && \
         DEBIAN_FRONTEND="noninteractive" \
         apt-get install -y --no-install-recommends \
         build-essential \
         cmake \
         ninja-build \
         ccache \
         git \
         curl \
         vim \
         wget \
         ca-certificates \
         tzdata \
         libssl-dev \
         gcc \
         g++ \
         gdb && \
         apt-get autoremove -y && \
         apt-get clean -y

# 3rdpart init
RUN mkdir -p /opt/media/ZLMediaKit/3rdpart
WORKDIR /opt/media/ZLMediaKit/3rdpart
RUN wget https://github.com/cisco/libsrtp/archive/v2.3.0.tar.gz -O libsrtp-2.3.0.tar.gz && \
    tar xfv libsrtp-2.3.0.tar.gz && \
    mv libsrtp-2.3.0 libsrtp && \
    cd libsrtp && CFLAGS="-fcommon" ./configure --enable-openssl && make -j $(nproc) && make install
#RUN git submodule update --init --recursive && \

COPY . /opt/media/ZLMediaKit
WORKDIR /opt/media/ZLMediaKit

RUN mkdir -p build release/linux/${MODEL}/

WORKDIR /opt/media/ZLMediaKit/build
RUN --mount=type=cache,target=/root/.cache/ccache \
        ccache -M 2G && \
        cmake -G Ninja \
            -DENABLE_PYTHON=false \
            -DCMAKE_BUILD_TYPE=${MODEL} \
            -DENABLE_WEBRTC=false \
            -DENABLE_FFMPEG=true \
            -DENABLE_TESTS=false \
            -DENABLE_API=false \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            .. && \
        cmake --build . --parallel $(nproc)

FROM ubuntu:24.04 AS runtime
ARG MODEL=Release

# ADD sources.list /etc/apt/sources.list

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
        --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
        apt-get update && \
        DEBIAN_FRONTEND="noninteractive" \
        apt-get install -y --no-install-recommends \
         ca-certificates \
         tzdata \
         ffmpeg \
        openssl && \
         apt-get autoremove -y && \
        apt-get clean -y

ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
        && echo $TZ > /etc/timezone && \
    mkdir -p /opt/media/bin/www /opt/media/conf /opt/media/bin/log

WORKDIR /opt/media/bin/
COPY --from=build /opt/media/ZLMediaKit/release/linux/${MODEL}/MediaServer /opt/media/ZLMediaKit/default.pem /opt/media/bin/
COPY --from=build /opt/media/ZLMediaKit/release/linux/${MODEL}/config.ini /opt/media/conf/
COPY --from=build /opt/media/ZLMediaKit/www/ /opt/media/bin/www/
COPY --from=build /opt/media/ZLMediaKit/tools/record_zlm_resources.sh /opt/media/bin/
RUN chmod +x /opt/media/bin/record_zlm_resources.sh

ENV PATH=/opt/media/bin:$PATH
CMD ["./MediaServer","-s", "default.pem", "-c", "../conf/config.ini", "--log-dir", "/opt/media/bin/log", "-l","0"]
