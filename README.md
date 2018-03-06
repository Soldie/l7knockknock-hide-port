# knock-ssh

Hide an ssh service behind another tcp port. For example behind a https server (port 443). Only when you receive a certain piece of text (or bytes) will the ssh port be unlocked.

A simplified alternative to port knocking, very handy for cases where your on restricted internet with only a few open ports.

## Developing

Since the api is quite linux specific, there is a custom docker image that can be used to build and run the knock-ssh application

    # prepare docker image
    docker build -t knock-ssh-build-env .

    # run docker image on windows
    docker run --rm -it -v "${PWD}:/root/build" knock-ssh-build-env

    # run docker image on osx/linux
    docker run --rm -it -v "$(pwd):/root/build" knock-ssh-build-env

    # inside the container (starts with bash)
    make clean && make test-splice 

    # you can also pass `make test-splice` directly to the run command
    docker run --rm -it -v "${PWD}:/root/build" knock-ssh-build-env make test-splice

