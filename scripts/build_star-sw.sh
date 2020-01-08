#!/usr/bin/env bash

# Set default values for primary parameters
: ${STAR_CVS_REF:="master"}
: ${STAR_BUILD_TYPE:="Release"}
: ${STAR_BUILD_32BIT:=""}
: ${STAR_BASE_IMAGE:="centos7"}
: ${STAR_SW_DIR:="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"}
: ${STAR_SW_REF:="master"}
: ${DOCKER_ID_NAMESPACE:="starbnl/"}


function print_usage() {
	echo "Usage: $0 [<star-cvs_branch_or_tag [=master]> ]" \
	              " [-t Debug|Release|RelWithDebInfo [=Release]]" \
	              " [-m [Force 32-bit builds on 64-bit platform]]" \
	              " [-b <star_base_image [=centos7]>]" \
	              " [-p <path_to_star-sw [=$STAR_SW_DIR]>]" \
	              " [-s <star-sw_branch_or_tag [=master]>]" 1>&2;
	exit 1;
}


# Process options and their values
options=$(getopt -o "ht:mb:p:s:" -n "$0" -- "$@")
[ $? -eq 0 ] || print_usage
eval set -- "$options"

while true; do
	case "$1" in
		-h)
			print_usage ;
			shift ;;
		-t)
			STAR_BUILD_TYPE="$2" ;
			[[ ! $STAR_BUILD_TYPE =~ Debug|Release|RelWithDebInfo ]] && {
				echo "Incorrect value provided for -t"
				print_usage
			}
			shift 2 ;;
		-m)
			STAR_BUILD_32BIT="Yes" ;
			shift 1 ;;
		-b)
			STAR_BASE_IMAGE="$2" ;
			shift 2 ;;
		-p)
			STAR_SW_DIR="$2" ;
			shift 2 ;;
		-s)
			STAR_SW_REF="$2" ;
			shift 2 ;;
		--)
			shift ;
			break ;;
	esac
done

# Process the only positional argument
if [ -n "$1" ]
then
	STAR_CVS_REF=$1
fi

# Just print out the variable's values for the record
echo "The following variables are set:"
echo -e "\t STAR_CVS_REF:          \"$STAR_CVS_REF\""
echo -e "\t STAR_BUILD_TYPE:       \"$STAR_BUILD_TYPE\""
echo -e "\t STAR_BUILD_32BIT:      \"$STAR_BUILD_32BIT\""
echo -e "\t STAR_BASE_IMAGE:       \"$STAR_BASE_IMAGE\""
echo -e "\t STAR_SW_DIR:           \"$STAR_SW_DIR\""
echo -e "\t STAR_SW_REF:           \"$STAR_SW_REF\""
echo -e "\t DOCKER_ID_NAMESPACE:   \"$DOCKER_ID_NAMESPACE\""

# Format image name as star-sw:$STAR_CVS_REF-$STAR_BUILD_TYPE-sw-$STAR_SW_REF
STAR_BASE_IMAGE_TAG="$STAR_BASE_IMAGE"
STAR_BASE_IMAGE_TAG+=$([ -z "$STAR_BUILD_32BIT" ] && echo "" || echo "-m32")

# Format image name as star-sw:$STAR_CVS_REF-$STAR_BUILD_TYPE-sw-$STAR_SW_REF
STAR_IMAGE_TAG=$([ "$STAR_CVS_REF" = "master" ] && echo "latest" || echo "$STAR_CVS_REF")
STAR_IMAGE_TAG+=$([ "$STAR_BUILD_TYPE" = "Release" ] && echo "" || echo "-$STAR_BUILD_TYPE")
STAR_IMAGE_TAG+=$([ "$STAR_BASE_IMAGE_TAG" = "centos7" ] && echo "" || echo "-$STAR_BASE_IMAGE_TAG")
STAR_IMAGE_TAG+=$([ "$STAR_SW_REF"  = "master" ] && echo "" || echo "-sw-$STAR_SW_REF")

echo
echo -e "\t STAR_BASE_IMAGE_TAG:   \"$STAR_BASE_IMAGE_TAG\""
echo -e "\t STAR_IMAGE_TAG:        \"$STAR_IMAGE_TAG\""

cmd="docker build --rm -t ${DOCKER_ID_NAMESPACE}star-base:${STAR_BASE_IMAGE_TAG} \
    -f ${STAR_SW_DIR}/docker/Dockerfile.star-base-${STAR_BASE_IMAGE} \
    --build-arg STAR_BUILD_32BIT=${STAR_BUILD_32BIT} \
    ${STAR_SW_DIR}
"
echo
echo $ $cmd
$cmd

cmd="docker build --rm -t ${DOCKER_ID_NAMESPACE}star-sw-build:${STAR_IMAGE_TAG} \
    -f ${STAR_SW_DIR}/docker/Dockerfile.star-sw \
    --build-arg STAR_CVS_REF=${STAR_CVS_REF} \
    --build-arg STAR_BUILD_TYPE=${STAR_BUILD_TYPE} \
    --build-arg STAR_BASE_IMAGE=${DOCKER_ID_NAMESPACE}star-base:${STAR_BASE_IMAGE_TAG} \
    --build-arg STAR_SW_REF=${STAR_SW_REF} \
    --target build-stage \
    ${STAR_SW_DIR}
"
echo
echo $ $cmd
$cmd

cmd="docker build --rm -t ${DOCKER_ID_NAMESPACE}star-sw:${STAR_IMAGE_TAG} \
    -f ${STAR_SW_DIR}/docker/Dockerfile.star-sw \
    --build-arg STAR_CVS_REF=${STAR_CVS_REF} \
    --build-arg STAR_BUILD_TYPE=${STAR_BUILD_TYPE} \
    --build-arg STAR_BASE_IMAGE=${DOCKER_ID_NAMESPACE}star-base:${STAR_BASE_IMAGE_TAG} \
    --build-arg STAR_SW_REF=${STAR_SW_REF} \
    ${STAR_SW_DIR}
"
echo
echo $ $cmd
$cmd

cmd="docker build --rm -t ${DOCKER_ID_NAMESPACE}star-sw-cons:${STAR_IMAGE_TAG} \
    -f ${STAR_SW_DIR}/docker/Dockerfile.star-sw-cons \
    --build-arg STAR_CVS_REF=${STAR_CVS_REF} \
    --build-arg STAR_BUILD_TYPE=${STAR_BUILD_TYPE} \
    --build-arg STAR_BUILD_32BIT=${STAR_BUILD_32BIT} \
    --build-arg STAR_BASE_IMAGE=${DOCKER_ID_NAMESPACE}star-base:${STAR_BASE_IMAGE_TAG} \
    ${STAR_SW_DIR}
"
echo
echo $ $cmd
$cmd