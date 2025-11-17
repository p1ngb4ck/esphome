ARG BUILD_BASE_VERSION=2025.04.0


FROM ghcr.io/esphome/docker-base:debian-${BUILD_BASE_VERSION} AS base

RUN git config --system --add safe.directory "*"

RUN apt update \
    && apt install -y \
    build-essential \
    sudo \
    libusb-1.0-0-dev \
    udev \
    protobuf-compiler \
    apt-utils \
    bison \
    ca-certificates \
    ccache \
    check \
    curl \
    flex \
    git \
    gperf \
    lcov \
    libffi-dev \
    libncurses-dev \
    libusb-1.0-0-dev \
    make \
    nano \
    ninja-build \
    python3 \
    python3-pip \
    unzip \
    wget \
    xz-utils \
    zip

RUN apt-get autoremove -y
RUN apt-get clean
RUN python -m pip install --upgrade \
    pip \
    virtualenv \
    setuptools \
    wheel

RUN pip install uv
RUN useradd esphome -m
RUN usermod -aG sudo esphome
RUN usermod -aG dialout esphome
RUN usermod -aG plugdev esphome
RUN usermod -aG users esphome
RUN echo "esphome ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers
RUN mkdir -p /home/esphome/.ccache
RUN chown -R esphome:esphome /home/esphome/.ccache
RUN ln -s /opt/esp /home/esphome/.espressif
RUN chown -R esphome:esphome /home/esphome/.espressif
RUN chown root:users /opt
RUN chmod 0770 /opt
USER esphome
ENV CCACHE_DIR=/home/esphome/.ccache
#ENV VIRTUAL_ENV=/home/esphome/.local/esphome-venv
#RUN uv venv $VIRTUAL_ENV
#ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Override this set to true in the docker-base image
ENV UV_SYSTEM_PYTHON=false

WORKDIR /tmp


# To build the image for a branch or a tag of IDF, pass --build-arg IDF_CLONE_BRANCH_OR_TAG=name.
# To build the image with a specific commit ID of IDF, pass --build-arg IDF_CHECKOUT_REF=commit-id.
# It is possibe to combine both, e.g.:
#   IDF_CLONE_BRANCH_OR_TAG=release/vX.Y
#   IDF_CHECKOUT_REF=<some commit on release/vX.Y branch>.

ARG IDF_CLONE_URL=https://github.com/espressif/esp-idf.git
ARG IDF_CLONE_BRANCH_OR_TAG=v5.5.1
ARG IDF_CHECKOUT_REF=

ENV IDF_PATH=/opt/esp/idf
ENV IDF_TOOLS_PATH=/opt/esp

RUN echo IDF_CHECKOUT_REF=$IDF_CHECKOUT_REF IDF_CLONE_BRANCH_OR_TAG=$IDF_CLONE_BRANCH_OR_TAG && \
    git clone --recursive \
    ${IDF_CLONE_BRANCH_OR_TAG:+-b $IDF_CLONE_BRANCH_OR_TAG} \
    $IDF_CLONE_URL $IDF_PATH && \
    if [ -n "$IDF_CHECKOUT_REF" ]; then \
    cd $IDF_PATH && \
    git checkout $IDF_CHECKOUT_REF && \
    git submodule update --init --recursive; \
    fi

# Install all the required tools
RUN : \
    && sudo update-ca-certificates --fresh \
    && $IDF_PATH/tools/idf_tools.py --non-interactive install required \
    && $IDF_PATH/tools/idf_tools.py --non-interactive install cmake \
    && $IDF_PATH/tools/idf_tools.py --non-interactive install-python-env \
    && rm -rf $IDF_TOOLS_PATH/dist \
    && :

# Ccache is installed, enable it by default
ENV IDF_CCACHE_ENABLE=1
ENV PATH="$IDF_TOOLS_PATH:$PATH"
RUN uv venv /opt/esp/esphome-venv
ENV PATH="/opt/esp/esphome-venv/bin:$PATH"
COPY requirements.txt ./
RUN uv pip install -r requirements.txt
COPY requirements_dev.txt requirements_test.txt ./
RUN uv pip install -r requirements_dev.txt -r requirements_test.txt

RUN \
    platformio settings set enable_telemetry No \
    && platformio settings set check_platformio_interval 1000000

COPY script/platformio_install_deps.py platformio.ini ./
RUN ./platformio_install_deps.py platformio.ini --libraries --platforms --tools

WORKDIR /workspaces
CMD [ "/bin/bash" ]
