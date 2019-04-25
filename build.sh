#/bin/bash

DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

BUILD_PATH=${PWD}
SRC_PATH=$(realpath $(dirname "$0"))
echo "SRC path  ="$SRC_PATH
echo "Build path="${BUILD_PATH}

redis_unix_socket="/tmp/redis.sock"

print_help() {
    usage_text="Usage: $(basename "$0") [option] -- Swss build script
options:
    -h, --help                            Show this help text
    -c, --clean                           Clean all genererated files"

    echo "$usage_text"
}

apply_patch() {
    local files i

    # sonic-swss-common
    sed -i '/CFLAGS_COMMON+=" -Werror"/d' "${SRC_PATH}/sonic-swss-common/configure.ac"
    sed -i 's|-I../common|-I$(top_srcdir)/common|g' "${SRC_PATH}/sonic-swss-common/pyext/Makefile.am"
    sed -i 's|_swsscommon_la_LIBADD = ../common/libswsscommon.la|_swsscommon_la_LIBADD = $(top_builddir)/common/libswsscommon.la|g' "${SRC_PATH}/sonic-swss-common/pyext/Makefile.am"
    sed -i 's|-L$(top_srcdir)/common|-L$(top_builddir)/common|g' "${SRC_PATH}/sonic-swss-common/tests/Makefile.am"

    sed -i "s|/var/run/redis/redis.sock|$redis_unix_socket|g" "${SRC_PATH}/sonic-swss-common/common/dbconnector.h"
    
    # sonic-sairedis
    sed -i '/CFLAGS_COMMON+=" -Werror"/d' "${SRC_PATH}/sonic-sairedis/configure.ac"
    sed -i '/-Wmissing-include-dirs \\/d' "${SRC_PATH}/sonic-sairedis/meta/Makefile.am"

    if ! grep -q tests_DEPENDENCIES "${SRC_PATH}/sonic-sairedis/meta/Makefile.am"; then
        sed -i '/tests_LDADD = -lhiredis -lswsscommon/atests_DEPENDENCIES = libsaimeta.la libsaimetadata.la' "${SRC_PATH}/sonic-sairedis/meta/Makefile.am"
    fi

    if ! grep -q tests_DEPENDENCIES "${SRC_PATH}/sonic-sairedis/vslib/src/Makefile.am"; then
        sed -i '/tests_LDADD = -lhiredis -lswsscommon/atests_DEPENDENCIES = libsaivs.la' "${SRC_PATH}/sonic-sairedis/vslib/src/Makefile.am"
    fi

    files=("${SRC_PATH}/sonic-sairedis/meta/Makefile.am" "${SRC_PATH}/sonic-sairedis/vslib/src/Makefile.am")
    files+=("${SRC_PATH}/sonic-sairedis/saidiscovery/Makefile.am")
    files+=("${SRC_PATH}/sonic-sairedis/saidump/Makefile.am")
    files+=("${SRC_PATH}/sonic-sairedis/saiplayer/Makefile.am")
    files+=("${SRC_PATH}/sonic-sairedis/saisdkdump/Makefile.am")
    files+=("${SRC_PATH}/sonic-sairedis/syncd/Makefile.am")
    files+=("${SRC_PATH}/sonic-sairedis/tests/Makefile.am")
    for i in "${files[@]}"
    do
	    sed -i 's|-L$(top_srcdir)|-L$(top_builddir)|g' "$i"
    done

    # sonic-swss
    sed -i 's|CFLAGS_COMMON+=" -Werror"|#CFLAGS_COMMON+=" -Werror"|g' "${SRC_PATH}/sonic-swss/configure.ac"

    sed -i 's|string str = counterIdsToStr(c_portStatIds, &sai_serialize_port_stat);|string str = counterIdsToStr(c_portStatIds, static_cast<string (*)(const sai_port_stat_t)>(\&sai_serialize_port_stat));|g' "${SRC_PATH}/sonic-swss/orchagent/pfcwdorch.cpp"
    sed -i 's|string str = counterIdsToStr(c_queueStatIds, sai_serialize_queue_stat);|string str = counterIdsToStr(c_queueStatIds, static_cast<string (*)(const sai_queue_stat_t)>(\&sai_serialize_queue_stat));|g' "${SRC_PATH}/sonic-swss/orchagent/pfcwdorch.cpp"

    # original cmd: `patch -N (acl/crm/pfcwd)orch.h < swss-(acl/crm)orch.diff`
    patch -N "${SRC_PATH}/sonic-swss/orchagent/aclorch.h" < "${SRC_PATH}/patch/swss-aclorch.diff"
    rm aclorch.h.rej
    patch -N "${SRC_PATH}/sonic-swss/orchagent/crmorch.h" < "${SRC_PATH}/patch/swss-crmorch.diff"
    rm crmorch.h.rej
    patch -N "${SRC_PATH}/sonic-swss/orchagent/pfcwdorch.h" < "${SRC_PATH}/patch/swss-pfcwdorch.diff"
    rm pfcwdorch.h.rej
    
}

download_source_code() {
    cd "${SRC_PATH}"
    git submodule update --init --recursive

    apply_patch
}

download_deps() {
    cd "$BUILD_PATH"
    [ -f "$BUILD_PATH/packages/.env" ] || $DIR/install-pkg.sh

    [ -f "$BUILD_PATH/packages/.env" ] || exit 1

    . "$BUILD_PATH/packages/.env"
}

generate_redis_config() {
    mkdir -p "$BUILD_PATH/redis"

    cp "$SRC_PATH/redis.conf" "$BUILD_PATH/redis"
    
    sed -i "s|/mnt/e/xx|$BUILD_PATH|g" "$BUILD_PATH/redis/redis.conf"
    sed -i "s|# unixsocket /var/run/redis/redis-server.sock|unixsocket $redis_unix_socket|g" "$BUILD_PATH/redis/redis.conf"

    cp "$SRC_PATH/start_redis.sh" "$BUILD_PATH/redis"
    chmod +x "$BUILD_PATH/redis/start_redis.sh"

    sed -i "s|/mnt/e/xx/redis.conf|$BUILD_PATH/redis/redis.conf|g" "$BUILD_PATH/redis/start_redis.sh"

    cp "$SRC_PATH/stop_redis.sh" "$BUILD_PATH/redis"
    chmod +x "$BUILD_PATH/redis/stop_redis.sh"

    sed -i "s|%redis_unix_socket%|$redis_unix_socket|g" "$BUILD_PATH/redis/stop_redis.sh"
}

build_swsscommon() {
    echo "Building sonic-swss-common ..."

    SWSS_COMMON_PATH="${SRC_PATH}/sonic-swss-common"

    mkdir -p "${BUILD_PATH}/sonic-swss-common"
    cd "${BUILD_PATH}/sonic-swss-common"

    # if [ ! -f "${SRC_PATH}/sonic-swss-common/configure" ]; then
    if [ ! -e "${BUILD_PATH}/sonic-swss-common/Makefile" ]; then
        # cd ${SWSS_COMMON_PATH}
        cd "${SRC_PATH}/sonic-swss-common"
        ./autogen.sh
        # make distclean

        # mkdir -p "${BUILD_PATH}/sonic-swss-common"
        cd "${BUILD_PATH}/sonic-swss-common"

        "${SRC_PATH}/sonic-swss-common/configure" --prefix=${BUILD_PATH}/install
    fi

    # cd "${SRC_PATH}/sonic-swss-common"
    # make distclean

    # mkdir -p "${BUILD_PATH}/sonic-swss-common"
    # cd "${BUILD_PATH}/sonic-swss-common"
    # "${SRC_PATH}/sonic-swss-common/configure" --prefix=$(realpath ${BUILD_PATH}/install )
    make "-j$(nproc)"

    if [ "$?" -ne "0" ]; then
        echo "Failed to build swss-common"
        exit 1
    fi

    # TODO: No need to do this ?
    # TODO: Add variables to collect includedirs and libs for swss(commom/sairedis/orchagent ...) and passing to configure or just add into .env
    make install

    # TODO: Remove ?
    rm -rf "${SRC_PATH}/sonic-swss-common/m4"
    rm -rf "${SRC_PATH}/sonic-swss-common/autom4te.cache"
    rm -rf "${SRC_PATH}/sonic-swss-common/config.h.in~"

    # TODO: No need to do this
    cd "${SRC_PATH}/sonic-swss-common"
    cp ./common/*.h ${BUILD_PATH}/install/include/swss
    cp ./common/*.hpp ${BUILD_PATH}/install/include/swss
}

build_sairedis() {
    echo "Building sonic-sairedis ..."

    SAIREDIS_PATH="${SRC_PATH}/sonic-sairedis"

    if [ ! -e "${BUILD_PATH}/sonic-sairedis/Makefile" ]; then
        cd "${SRC_PATH}/sonic-sairedis"
        ./autogen.sh

        mkdir -p "${BUILD_PATH}/sonic-sairedis"
        cd "${BUILD_PATH}/sonic-sairedis"

        #                                                                                     for #include "meta/sai_meta.h" 
        "${SAIREDIS_PATH}/configure" --prefix=${BUILD_PATH}/install --with-sai=vs CXXFLAGS="-g -O0 -I${SAIREDIS_PATH} -I${BUILD_PATH}/install/include \
        -Wno-error=long-long \
        -std=c++11 \
        -L${BUILD_PATH}/install/lib $CXXFLAGS"
    fi

    echo "Generating saimetadata.c and saimetadata.h ..."

    cd ${SAIREDIS_PATH}/SAI/meta
    export PERL5LIB=${PWD}
    make saimetadata.c saimetadata.h
    if [ "$?" -ne "0" ]; then
        echo "Failed to build saimetadata"
        exit 1
    fi

    echo "Build sairedis ..."

    mkdir -p "${BUILD_PATH}/sonic-sairedis"
    cd "${BUILD_PATH}/sonic-sairedis"

    make "-j$(nproc)"
    if [ "$?" -ne "0" ]; then
        echo "Failed to build sairedis"
        exit 1
    fi

    # TODO: No need to do this
    # TODO: Add variables to collect includedirs and libs for swss(commom/sairedis/orchagent ...) and passing to configure or just add into .env
    make install
}

build_swss_orchagent() {
    echo "Build sonic-swss-orchagent ..."

    SWSS_PATH="${SRC_PATH}/sonic-swss"
    mkdir -p "${BUILD_PATH}/sonic-swss"
    cd "${BUILD_PATH}/sonic-swss"

    if [ ! -e "${BUILD_PATH}/sonic-swss/Makefile" ]; then
        cd "${SRC_PATH}/sonic-swss"
        ./autogen.sh

        cd "${BUILD_PATH}/sonic-swss"

        "${SWSS_PATH}/configure" --prefix=${BUILD_PATH}/install --with-sai=vs CXXFLAGS="-g -O0 -I${SWSS_PATH} \
        -I${BUILD_PATH}/install/include/swss \
        -I${BUILD_PATH}/install/include/ \
        -I${SRC_PATH}/sonic-sairedis/SAI/inc \
        -I${SRC_PATH}/sonic-sairedis/lib/inc \
        -I${SRC_PATH}/sonic-sairedis/meta \
        -I${SRC_PATH}/sonic-sairedis/SAI/meta \
        -I${SRC_PATH}/sonic-sairedis/SAI/experimental \
        -I${SRC_PATH}/sonic-swss/orchagent \
        -Wno-error=long-long -std=c++11 \
        -L${BUILD_PATH}/install/lib $CXXFLAGS"
    fi

    echo "Build swss ..."
    
    cd ${BUILD_PATH}/sonic-swss
    make "-j$(nproc)" && make -C ./tests

    if [ "$?" -ne "0" ]; then
        echo "Failed to build swss"
        exit 1
    fi

    ##################################

    cd ${BUILD_PATH}
    cmake ${SRC_PATH} -DCMAKE_CXX_FLAGS="-I${SAIREDIS_PATH}/vslib/inc $CXXFLAGS $LIBS" -DGTEST_ROOT_DIR=$(pkg-config --variable=prefix googletest) -DREDIS_START_CMD="$BUILD_PATH/redis/start_redis.sh" -DREDIS_STOP_CMD="$BUILD_PATH/redis/stop_redis.sh"
    make "-j$(nproc)"
}

build_all() {
    build_swsscommon
    build_sairedis
    build_swss_orchagent
}

clean_all() {
    rm -rf "${SRC_PATH}/sonic-swss-common/m4"
    rm -rf "${SRC_PATH}/sonic-swss-common/autom4te.cache"
    rm -rf "${SRC_PATH}/sonic-swss-common/config.h.in~"
}

main() {
    while [[ $# -ne 0 ]]
    do
        arg="$1"
        case "$arg" in
            -h|--help)
                print_help
                exit 0
                ;;
            -c|--clean)
                clean_all
                exit 0
                ;;
            *)
                echo >&2 "Invalid option \"$arg\""
                print_help
                exit 1
        esac
        shift
    done

    download_source_code
    download_deps
    generate_redis_config

    # TODO: can remove this ??
    cd $SRC_PATH
    mkdir -p ${BUILD_PATH}/install/include/swss

    build_all
}

main "$@"
