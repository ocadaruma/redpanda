FROM fedora:36

COPY --chown=0:0 install-dependencies.sh /

# We need to update clang version due to https://github.com/redpanda-data/redpanda/issues/5860#issuecomment-1210704853
# We enable fedora's snapshot repo to install clang 16 (this also need to use newer fedora image than 34)
RUN dnf install 'dnf-command(copr)' -y
RUN dnf copr enable -y @fedora-llvm-team/llvm-snapshots
# python3.10 is the default but some osses doesn't compile with 3.10
RUN dnf install python39 -y
RUN /install-dependencies.sh && \
    dnf clean all && \
    rm install-dependencies.sh
