FROM ubuntu:focal

RUN export DEBIAN_FRONTEND=noninteractive && \
    apt-get update -y && \
    apt-get install -y \
        python3 python3-pip cmake llvm-12-dev clang-12 lld-12

RUN mkdir -p /opt/dataflow
WORKDIR /opt/dataflow
ADD CMakeLists.txt .
ADD ext ext
ADD include include
ADD lib lib
ADD tools tools

#RUN git clone https://github.com/HexHive/datAFLow /opt/dataflow && \
#    cd /opt/dataflow && \
#    git submodule update --init

ARG BUILD_TYPE=Release
RUN cd /opt/dataflow && \
    cd ext/aflplusplus && \
    make clean
RUN cd /opt/dataflow && \
    mkdir -p build && \
    cd build && \
    cmake .. -DLLVM_DIR=`llvm-config-12 --cmakedir` \
        -DCMAKE_C_COMPILER=clang-12 -DCMAKE_CXX_COMPILER=clang++-12 \
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE && \
    make clean && \
    make -j && \
    make -j install
