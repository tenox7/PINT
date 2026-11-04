# tenox7/pint40sdk: the PINT 4.0 SDK on msvc-arm32, Microsoft C for ARM under Wine (which
# msvc-setup installs into a volume from Microsoft). Its cl, link, nmake and rc build for
# Windows NT 4.0 on ARM with the SDK's headers and libraries (/opt/pint40sdk): cl and link
# are the SDK's (wine-sdk), nmake, rc and the other tools msvc-arm32's, in the SDK's
# environment (pint-env).
FROM tenox7/msvc-arm32
COPY pint40sdk /opt/pint40sdk
COPY pint-env pint-entrypoint wine-sdk cl link /usr/local/bin/
ENTRYPOINT ["pint-entrypoint"]
WORKDIR /src
