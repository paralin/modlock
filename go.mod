module github.com/paralin/modlock

go 1.27.0

replace (
	github.com/cloudflare/circl => github.com/aperturerobotics/circl v1.6.4-0.20261026102657-7415ab3046b7
	github.com/pion/ice/v4 => github.com/aperturerobotics/pion-ice/v4 v4.0.0-20260923005200-d591ff5895c0
	github.com/pion/webrtc/v4 => github.com/aperturerobotics/pion-webrtc/v4 v4.2.16-0.20260922235615-c5c6104de130
	github.com/sirupsen/logrus => github.com/aperturerobotics/logrus v1.10.2-0.20260922042813-73f013325281 // master
)

require (
	github.com/aperturerobotics/abseil-cpp v0.0.0-20260131110040-4bb56e2f9017
	github.com/aperturerobotics/cli v1.1.1-0.20261008170756-47089de94fb3
	github.com/aperturerobotics/common v0.36.4
	github.com/aperturerobotics/controllerbus v0.53.6-0.20261021005226-3d7ed787e11b
	github.com/aperturerobotics/fastjson v0.1.2-0.20260705010846-94f343f5bb34
	github.com/aperturerobotics/go-protoc-wasi v0.0.0-20260808023521-7b1595380c3f
	github.com/aperturerobotics/go-websocket v1.8.15-0.20260910142736-349504d079da
	github.com/aperturerobotics/protobuf v0.0.0-20260203024654-8201686529c4
	github.com/aperturerobotics/protobuf-go-lite v0.19.1-0.20261018054529-f60c4d2691ea
	github.com/aperturerobotics/starpc v0.52.4-0.20261021005057-f60467d0da35
	github.com/aperturerobotics/util v1.34.10-0.20261021005056-6e84f021eba1
	github.com/evanw/esbuild v0.28.2
	github.com/fsnotify/fsnotify v1.9.0
	github.com/klauspost/compress v1.20.3
	github.com/pierrec/lz4/v4 v4.1.33
	github.com/pkg/errors v0.9.1
	github.com/s4wave/spacewave v0.57.29-0.20261023040135-ed58e42ab255
	github.com/sirupsen/logrus v1.10.2
	github.com/tetratelabs/wazero v1.12.1-0.20261019203559-ef2dd2f85376
	golang.org/x/mod v0.42.0
	golang.org/x/sys v0.49.0
	golang.org/x/term v0.47.0
)

require (
	filippo.io/edwards25519 v1.2.0 // indirect
	github.com/ProtonMail/go-crypto v1.5.2 // indirect
	github.com/aperturerobotics/bbolt v0.0.0-20261006081945-09141165d72c // indirect
	github.com/aperturerobotics/cayley v0.15.1-0.20261021230328-e1c6a005d6ac // indirect
	github.com/aperturerobotics/fsnotify v1.9.1-0.20261007093243-45a75ed654f0 // indirect
	github.com/aperturerobotics/go-brotli-decoder v1.2.2 // indirect
	github.com/aperturerobotics/go-multiaddr v0.17.1-0.20260514224402-c193991c3ce5 // indirect
	github.com/aperturerobotics/go-multibase v0.4.0 // indirect
	github.com/aperturerobotics/go-protoc-gen-prost v0.0.0-20261003014807-d46d22154f35 // indirect
	github.com/aperturerobotics/json-iterator-lite v1.1.0 // indirect
	github.com/bwesterb/go-ristretto v1.2.4 // indirect
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/cloudflare/circl v1.6.5 // indirect
	github.com/dgraph-io/ristretto/v2 v2.4.2 // indirect
	github.com/dolthub/vitess v0.0.0-20261014211039-3e903c0f05d3 // indirect
	github.com/dustin/go-humanize v1.1.0 // indirect
	github.com/ghodss/yaml v1.0.0 // indirect
	github.com/go-git/gcfg/v2 v2.0.2 // indirect
	github.com/go-git/go-billy/v6 v6.0.0-beta.1.0.20261016211742-fd38f5ac32fe // indirect
	github.com/golang/protobuf v1.5.4 // indirect
	github.com/gomodule/redigo v2.0.0+incompatible // indirect
	github.com/google/uuid v1.6.0 // indirect
	github.com/hack-pad/safejs v0.1.1 // indirect
	github.com/klauspost/cpuid/v2 v2.3.0 // indirect
	github.com/libp2p/go-buffer-pool v0.1.0 // indirect
	github.com/libp2p/go-yamux/v5 v5.1.0 // indirect
	github.com/mattn/go-isatty v0.0.24 // indirect
	github.com/mattn/go-sqlite3 v2.0.3+incompatible // indirect
	github.com/minio/sha256-simd v1.0.1 // indirect
	github.com/mr-tron/base58 v1.3.0 // indirect
	github.com/multiformats/go-multihash v0.2.3 // indirect
	github.com/multiformats/go-varint v0.0.7 // indirect
	github.com/ncruces/go-sqlite3 v0.35.6 // indirect
	github.com/ncruces/go-sqlite3-wasm/v6 v6.3.35304 // indirect
	github.com/ncruces/go-strftime v1.1.0 // indirect
	github.com/ncruces/julianday v1.0.0 // indirect
	github.com/pion/datachannel v1.6.3 // indirect
	github.com/pion/dtls/v3 v3.1.9 // indirect
	github.com/pion/ice/v4 v4.4.4 // indirect
	github.com/pion/interceptor v0.1.49 // indirect
	github.com/pion/logging v0.2.4 // indirect
	github.com/pion/mdns/v2 v2.2.1 // indirect
	github.com/pion/randutil v0.1.0 // indirect
	github.com/pion/rtcp v1.2.17 // indirect
	github.com/pion/rtp v1.10.5 // indirect
	github.com/pion/sctp v1.11.3 // indirect
	github.com/pion/sdp/v3 v3.0.20 // indirect
	github.com/pion/srtp/v3 v3.1.0 // indirect
	github.com/pion/stun/v4 v4.0.1 // indirect
	github.com/pion/transport/v5 v5.1.1 // indirect
	github.com/pion/turn/v5 v5.1.2 // indirect
	github.com/pion/webrtc/v4 v4.2.25 // indirect
	github.com/quic-go/quic-go v0.64.1 // indirect
	github.com/remyoudompheng/bigfft v0.0.0-20230129092748-24d4a6f8daec // indirect
	github.com/restic/chunker v0.5.0 // indirect
	github.com/spaolacci/murmur3 v1.1.0 // indirect
	github.com/tidwall/btree v1.8.2 // indirect
	github.com/wlynxg/anet v0.0.5 // indirect
	github.com/xrash/smetrics v0.0.0-20250705151800-55b8f293f342 // indirect
	github.com/zeebo/blake3 v0.2.4 // indirect
	golang.org/x/crypto v0.58.0 // indirect
	golang.org/x/exp v0.0.0-20261014143641-f0251eb4aa3e // indirect
	golang.org/x/net v0.61.0 // indirect
	golang.org/x/sync v0.24.0 // indirect
	golang.org/x/time v0.15.0 // indirect
	google.golang.org/genproto v0.0.0-20230410155749-daa745c078e1 // indirect
	google.golang.org/grpc v1.56.3 // indirect
	google.golang.org/protobuf v1.36.12 // indirect
	gopkg.in/yaml.v2 v2.4.0 // indirect
	gopkg.in/yaml.v3 v3.0.1 // indirect
	lukechampine.com/blake3 v1.2.1 // indirect
	modernc.org/libc v1.79.0 // indirect
	modernc.org/mathutil v1.7.1 // indirect
	modernc.org/memory v1.12.1 // indirect
	modernc.org/sqlite v1.61.0 // indirect
)
