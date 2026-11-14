FROM i386/debian:bookworm-slim

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends openssh-client

ENTRYPOINT ["ssh"]
