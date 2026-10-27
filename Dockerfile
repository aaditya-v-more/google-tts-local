FROM ubuntu:24.04

# The engine is built against glibc and LLVM's libc++. LIEF patches it once at fetch time.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libc++1 libc++abi1 python3 python3-pip ca-certificates \
 && pip install --no-cache-dir --break-system-packages lief==0.16.6 \
 && apt-get purge -y python3-pip && apt-get autoremove -y \
 && rm -rf /var/lib/apt/lists/*

COPY app/gtts.py /opt/gtts/gtts.py
VOLUME /data
WORKDIR /work
EXPOSE 5591
ENTRYPOINT ["python3", "/opt/gtts/gtts.py"]
