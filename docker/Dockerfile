# Local ZMK build environment for mi_corne
#
# Uses the same base image the GitHub Actions workflow uses
# (zmkfirmware/zmk-build-arm:stable), so local builds match CI byte-for-byte.
FROM zmkfirmware/zmk-build-arm:stable

COPY build.sh /opt/zmk-build/build.sh
RUN chmod +x /opt/zmk-build/build.sh

WORKDIR /build-env
ENTRYPOINT ["/opt/zmk-build/build.sh"]
