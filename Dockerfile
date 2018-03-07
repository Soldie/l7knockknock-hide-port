FROM ubuntu:16.04 

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get -y update && \
    apt-get install -y python-software-properties software-properties-common && \
    add-apt-repository ppa:longsleep/golang-backports && \
    apt-get -y update  && \
    apt-get install -y \
        build-essential \
        valgrind \
        git \
        libevent-dev \
        golang-go \
        vim \
    && rm -rf /var/lib/apt/lists/* && \
    go get github.com/gosuri/uiprogress && \
    go get github.com/cespare/xxhash

RUN mkdir -p /root/build
VOLUME /root/build

WORKDIR /root/build
CMD /bin/bash
