# l7knockknock: hiding one application behind another tcp port
[![Build Status](https://travis-ci.org/DavyLandman/l7knockknock.svg?branch=master)](https://travis-ci.org/DavyLandman/l7knockknock) 
[![Coverity](https://scan.coverity.com/projects/15326/badge.svg)](https://scan.coverity.com/projects/davylandman-l7knockknock)
[![codecov](https://codecov.io/gh/DavyLandman/l7knockknock/branch/master/graph/badge.svg)](https://codecov.io/gh/DavyLandman/l7knockknock)

Hide an tcp server behind another tcp server. For example, hiding SSH server behind a HTTPS server (port 443). This is a nice way to VPN yourself out of a restricted internet connection. Port knocking is not feasible as those special ports might be blocked too.

All connections to `l7knockknock` will automatically be proxied to the normal port, except when the first bytes of the connection match a special user chosen set of bytes. In that case, the connection is forwarded to the hidden server.

Previously I was using a port multiplexer, but project such as shodan have discovered these hidden servers, and I started seeing multiple brute-force approaches. l7knockknock just adds a superficial layer of security by obscurity, so it won't make it that much safer for direct attacks, it just stops the broad scans of the whole internet.

## Performance

To increase performance of the proxying, l7knockknock uses splicing to get zero-copying performance. This means that there is almost no noticeable performance impact.

## Developing

Since the API is quite Linux specific, there is a custom Docker image that can be used to build and test l7knockknock application

    # prepare docker image
    docker build -t l7knockknock-build-env .

    # run docker image on windows
    docker run --rm -it -v "${PWD}:/root/build" l7knockknock-build-env

    # run docker image on osx/linux
    docker run --rm -it -v "$(pwd):/root/build" l7knockknock-build-env

    # inside the container (starts with bash)
    make clean && make test-splice 

    # you can also pass `make test-splice` directly to the run command
    docker run --rm -it -v "${PWD}:/root/build" l7knockknock-build-env make test-splice
