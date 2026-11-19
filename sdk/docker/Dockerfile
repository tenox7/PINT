# tenox7/pint35sdk: the PINT 3.5 SDK on msvc-arm32, Microsoft C for ARM under Wine (which
# msvc-setup installs into a volume from Microsoft). Its cl, link, nmake and rc build for
# Windows NT 3.5 on ARM with the SDK's headers and libraries (/opt/pint35sdk): cl and link
# are the SDK's (wine-sdk), nmake, rc and the other tools msvc-arm32's, in the SDK's
# environment (pint-env).
FROM tenox7/msvc-arm32
COPY pint35sdk /opt/pint35sdk
COPY pint-env pint-entrypoint wine-sdk cl link /usr/local/bin/
ENTRYPOINT ["pint-entrypoint"]
WORKDIR /src
