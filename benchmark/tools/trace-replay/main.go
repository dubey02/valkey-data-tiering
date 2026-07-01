package main

import (
	"context"
	"encoding/csv"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/valkey-io/valkey-go"
)

var gSetOnMiss bool
var gTwitterMode bool
var gSynthType string
var gSynthItems int

type Op struct {
	Timestamp int
	Type      string
	Key       string
	Size      int
	TTL       int
}

type Stats struct {
	hits        atomic.Int64
	misses      atomic.Int64
	errors      atomic.Int64
	latencies   []time.Duration
	mu          sync.Mutex
	checkpoints []Checkpoint
}

func (s *Stats) recordLatency(d time.Duration) {
	s.mu.Lock()
	s.latencies = append(s.latencies, d)
	s.mu.Unlock()
}

type Result struct {
	Scenario    string       `json:"scenario"`
	Clients     int          `json:"clients"`
	Speed       float64      `json:"speed"`
	Ops         int64        `json:"ops"`
	Errors      int64        `json:"errors"`
	DurationS   float64      `json:"duration_s"`
	Throughput  float64      `json:"throughput_ops_s"`
	HitRatio    float64      `json:"hit_ratio_pct"`
	P50Ms       float64      `json:"p50_ms"`
	P99Ms       float64      `json:"p99_ms"`
	P999Ms      float64      `json:"p999_ms"`
	P100Ms      float64      `json:"p100_ms"`
	Checkpoints []Checkpoint `json:"checkpoints,omitempty"`
}

type Checkpoint struct {
	OpsCompleted   int64   `json:"ops_completed"`
	ElapsedS       float64 `json:"elapsed_s"`
	Throughput     float64 `json:"throughput_ops_s"`
	P50Ms          float64 `json:"p50_ms"`
	P99Ms          float64 `json:"p99_ms"`
	P999Ms         float64 `json:"p999_ms"`
	HitRatio       float64 `json:"hit_ratio_pct"`
	// Server metrics
	UsedMemoryMB   float64 `json:"used_memory_mb"`
	TieredValues   int64   `json:"tiered_values"`
	TieredFetches  int64   `json:"tiered_fetches"`
	IntervalFetches int64  `json:"interval_fetches"`
	FetchRatioPct  float64 `json:"fetch_ratio_pct"`
	// NVMe I/O (delta since last checkpoint)
	WriteIOPS    float64 `json:"write_iops"`
	WriteMBs     float64 `json:"write_mb_s"`
	ReadIOPS     float64 `json:"read_iops"`
	ReadMBs      float64 `json:"read_mb_s"`
}

func main() {
	trace := flag.String("trace", "", "trace CSV file")
	host := flag.String("host", "localhost", "valkey host")
	port := flag.Int("port", 6399, "valkey port")
	clients := flag.Int("clients", 50, "concurrent workers")
	rps := flag.Int("rps", 0, "target RPS (0=use timestamps)")
	speed := flag.Float64("speed", 1.0, "replay speed multiplier (1=realtime, 0=unlimited)")
	ops := flag.Int("ops", 100000, "ops to replay")
	pop := flag.Bool("populate", false, "populate keys before replay")
	popKeys := flag.Int("pop-keys", 0, "max keys to populate (0=all from trace)")
	scenario := flag.String("scenario", "", "scenario label")
	jsonOut := flag.String("json", "", "append JSON result to file")
	popOnly := flag.Bool("pop-only", false, "only populate, skip replay")
	stream := flag.Bool("stream", false, "stream trace from disk (low memory, for large traces)")

	// Synthetic populate mode (no trace needed)
	synthKeys := flag.Int("synth-keys", 0, "populate N synthetic keys (no trace needed)")
	synthSize := flag.Int("synth-size", 1024, "value size for synthetic keys")
	synthPrefix := flag.String("synth-prefix", "key:", "key prefix for synthetic populate")
	synthKeysize := flag.Int("synth-keysize", 0, "pad synthetic keys to N bytes (0=no padding)")
	spill := flag.Bool("spill", false, "after synth populate, DEBUG SPILL all keys and wait for drain")
	synthRead := flag.Bool("synth-read", false, "GET each synthetic key (paced by -rps), record latency")
	synthType := flag.String("synth-type", "string", "synthetic value type: string|hash|list|set|zset|stream")
	synthItems := flag.Int("synth-items", 1, "items per compound key (hash/list/set/zset/stream)")

	pipelineBatch := flag.Int("pipeline", 512, "pipeline batch size for populate/replay")
	checkpoint := flag.Int("checkpoint", 500000, "record metrics every N ops (0=disabled)")
	setOnMiss := flag.Bool("set-on-miss", false, "SET key on first GET miss (inline populate)")
	twitter := flag.Bool("twitter", false, "Twitter/memcached trace format (timestamp,key,key_size,value_size,seq_num,op,ttl)")

	flag.Parse()
	gSetOnMiss = *setOnMiss
	gTwitterMode = *twitter
	gSynthType = *synthType
	gSynthItems = *synthItems

	addr := fmt.Sprintf("%s:%d", *host, *port)

	// Synthetic mode: populate (+optional spill), OR read-only.
	if *synthKeys > 0 {
		if *synthRead {
			ops := make([]Op, *synthKeys)
			readType := "GET"
			if *synthType != "string" {
				readType = "SYNTHREAD"
			}
			for i := range ops {
				ops[i] = Op{Type: readType, Key: synthKey(*synthPrefix, i, *synthKeysize)}
			}
			fmt.Printf("Reading %d synthetic keys (%d clients, rps=%d)...\n", *synthKeys, *clients, *rps)
			stats, elapsed := replay(addr, ops, *clients, *rps, 0, *checkpoint)
			res := buildResult(stats, elapsed, *clients, 0, *scenario)
			printResult(res)
			if *jsonOut != "" {
				appendJSON(*jsonOut, res)
			}
			return
		}
		doSyntheticPopulate(addr, *synthKeys, *synthSize, *synthPrefix, *synthKeysize, *pipelineBatch)
		if *spill {
			spillSynthKeys(addr, *synthKeys, *synthPrefix, *synthKeysize, *pipelineBatch)
		}
		if *popOnly || *spill {
			return
		}
	}

	if *trace == "" && *synthKeys == 0 {
		fmt.Fprintf(os.Stderr, "Error: -trace or -synth-keys required\n")
		os.Exit(1)
	}

	// Streaming mode — reads trace line-by-line, never loads into memory
	if *stream && *trace != "" {
		if *popOnly || *pop {
			streamPopulate(addr, *trace, *popKeys, *pipelineBatch)
			if *popOnly {
				return
			}
		}
		if !*popOnly {
			fmt.Printf("Streaming replay: %s (%d ops, %d clients, speed=%.1f)\n", *trace, *ops, *clients, *speed)
			stats, elapsed := streamReplay(addr, *trace, *ops, *clients, *speed, *checkpoint)
			res := buildResult(stats, elapsed, *clients, *speed, *scenario)
			printResult(res)
			if *jsonOut != "" {
				appendJSON(*jsonOut, res)
			}
		}
		return
	}

	var traceOps []Op
	if *trace != "" {
		fmt.Printf("Loading trace: %s (%d ops)\n", *trace, *ops)
		traceOps = loadTrace(*trace, *ops)
		fmt.Printf("  Loaded %d ops (trace spans %ds)\n", len(traceOps), traceOps[len(traceOps)-1].Timestamp)

		if *pop {
			doPopulatePipelined(addr, traceOps, *popKeys, *pipelineBatch)
		}
	}

	if *popOnly {
		return
	}

	if len(traceOps) == 0 {
		fmt.Println("No ops to replay.")
		return
	}

	mode := "timestamp-based"
	if *rps > 0 {
		mode = fmt.Sprintf("%d rps", *rps)
	} else if *speed == 0 {
		mode = "unlimited"
	} else {
		mode = fmt.Sprintf("%.1fx realtime", *speed)
	}
	fmt.Printf("Replaying %d ops with %d clients (%s)...\n", len(traceOps), *clients, mode)

	stats, elapsed := replay(addr, traceOps, *clients, *rps, *speed, *checkpoint)
	res := buildResult(stats, elapsed, *clients, *speed, *scenario)
	printResult(res)

	if *jsonOut != "" {
		appendJSON(*jsonOut, res)
	}
}

// mapTraceOp converts memcached trace op names to internal op types
func mapTraceOp(op string) string {
	switch strings.ToLower(op) {
	case "get", "gets":
		return "GET"
	case "set":
		return "SET"
	case "add":
		return "ADD"
	case "cas":
		return "CAS"
	case "delete":
		return "DEL"
	default:
		return "GET"
	}
}

func loadTrace(path string, maxOps int) []Op {
	f, err := os.Open(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	r := csv.NewReader(f)
	r.FieldsPerRecord = -1
	r.TrimLeadingSpace = true

	if !gTwitterMode {
		r.Read() // skip header (default format has one, twitter format does not)
	}

	var ops []Op
	for len(ops) < maxOps {
		row, err := r.Read()
		if err == io.EOF {
			break
		}
		if err != nil || len(row) < 4 {
			continue
		}

		if gTwitterMode {
			// Twitter format: timestamp,key,key_size,value_size,seq_num,op,ttl
			ts, _ := strconv.Atoi(strings.TrimRight(row[0], "\r"))
			key := strings.TrimRight(row[1], "\r")
			size, _ := strconv.Atoi(strings.TrimRight(row[3], "\r"))
			opType := "GET"
			ttl := 0
			if len(row) >= 6 {
				opType = mapTraceOp(strings.TrimRight(row[5], "\r"))
			}
			if len(row) >= 7 {
				ttl, _ = strconv.Atoi(strings.TrimRight(row[6], "\r"))
			}
			ops = append(ops, Op{Timestamp: ts, Type: opType, Key: key, Size: size, TTL: ttl})
		} else {
			// Default format: timestamp,op,key,size
			ts, _ := strconv.Atoi(strings.TrimRight(row[0], "\r"))
			opType := strings.TrimRight(row[1], "\r")
			key := strings.TrimRight(row[2], "\r")
			size, _ := strconv.Atoi(strings.TrimRight(row[3], "\r"))
			ops = append(ops, Op{Timestamp: ts, Type: opType, Key: key, Size: size})
		}
	}
	return ops
}

// Pipelined populate from trace — sends batches of SETs
func doPopulatePipelined(addr string, traceOps []Op, maxKeys, batchSize int) {
	client, err := valkey.NewClient(valkey.ClientOption{InitAddress: []string{addr}, DisableCache: true})
	if err != nil {
		fmt.Fprintf(os.Stderr, "populate connect error: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	seen := make(map[string]int)
	for _, op := range traceOps {
		if _, ok := seen[op.Key]; !ok {
			seen[op.Key] = op.Size
		}
		if maxKeys > 0 && len(seen) >= maxKeys {
			break
		}
	}

	fmt.Printf("  Populating %d keys (pipeline batch=%d)...\n", len(seen), batchSize)
	ctx := context.Background()
	batch := make(valkey.Commands, 0, batchSize)
	sent, errs := 0, 0
	start := time.Now()

	for key, size := range seen {
		val := strings.Repeat("x", max(size, 1))
		batch = append(batch, client.B().Set().Key(key).Value(val).Build())
		if len(batch) >= batchSize {
			resps := client.DoMulti(ctx, batch...)
			for _, r := range resps {
				if r.Error() != nil {
					errs++
				}
			}
			sent += len(batch)
			batch = batch[:0]
			if sent%50000 == 0 {
				elapsed := time.Since(start).Seconds()
				fmt.Printf("    %d/%d (%.0f keys/s, %d errors)\n", sent, len(seen), float64(sent)/elapsed, errs)
			}
		}
	}
	if len(batch) > 0 {
		resps := client.DoMulti(ctx, batch...)
		for _, r := range resps {
			if r.Error() != nil {
				errs++
			}
		}
		sent += len(batch)
	}

	elapsed := time.Since(start).Seconds()
	fmt.Printf("  Done. %d keys in %.1fs (%.0f keys/s, %d errors)\n", sent, elapsed, float64(sent)/elapsed, errs)
}

// Synthetic populate — no trace needed
// synthKey builds a deterministic key, optionally right-padded to keySize bytes.
// Pads with '_' (non-digit) so distinct indices never collide (e.g. "key:1" and
// "key:10" must not both pad to the same string).
func synthKey(prefix string, i, keySize int) string {
	k := fmt.Sprintf("%s%d", prefix, i)
	if keySize > len(k) {
		k += strings.Repeat("_", keySize-len(k))
	}
	return k
}

// spillSynthKeys issues DEBUG SPILL for every synthetic key as fast as possible
// (unbounded — saturation is the workload), counts rejections as a measured result,
// then polls INFO until in-flight spills drain.
func spillSynthKeys(addr string, numKeys int, prefix string, keySize, batchSize int) {
	client, err := valkey.NewClient(valkey.ClientOption{InitAddress: []string{addr}, DisableCache: true})
	if err != nil {
		fmt.Fprintf(os.Stderr, "spill connect error: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()
	ctx := context.Background()

	fmt.Printf("  Spilling %d keys via DEBUG SPILL (unbounded)...\n", numKeys)
	batch := make(valkey.Commands, 0, batchSize)
	rejected := 0
	flush := func() {
		for _, r := range client.DoMulti(ctx, batch...) {
			if r.Error() != nil {
				rejected++
			}
		}
		batch = batch[:0]
	}
	for i := 0; i < numKeys; i++ {
		batch = append(batch, client.B().Arbitrary("DEBUG", "SPILL").Args(synthKey(prefix, i, keySize)).Build())
		if len(batch) >= batchSize {
			flush()
		}
	}
	if len(batch) > 0 {
		flush()
	}
	accepted := numKeys - rejected
	fmt.Printf("  DEBUG SPILL: accepted=%d rejected=%d (%.2f%% rejected under load)\n",
		accepted, rejected, float64(rejected)/float64(numKeys)*100)
	if accepted == 0 {
		fmt.Fprintf(os.Stderr, "  FATAL: all spills rejected (is enable-debug-command set?)\n")
		os.Exit(1)
	}

	deadline := time.Now().Add(120 * time.Second)
	for {
		spilling, onFlash := int64(-1), int64(0)
		if info, err := client.Do(ctx, client.B().Info().Section("external_storage").Build()).ToString(); err == nil {
			for _, line := range strings.Split(info, "\n") {
				line = strings.TrimSpace(line)
				if v, ok := strings.CutPrefix(line, "num_items_spilling_to_ext_storage:"); ok {
					spilling, _ = strconv.ParseInt(v, 10, 64)
				} else if v, ok := strings.CutPrefix(line, "num_items_on_flash:"); ok {
					onFlash, _ = strconv.ParseInt(v, 10, 64)
				}
			}
		}
		fmt.Printf("    draining: spilling=%d on_flash=%d (accepted=%d)\n", spilling, onFlash, accepted)
		if spilling == 0 {
			fmt.Printf("  Spill drained: %d keys on flash.\n", onFlash)
			break
		}
		if time.Now().After(deadline) {
			fmt.Fprintf(os.Stderr, "  WARN: drain still in-flight at timeout (spilling=%d on_flash=%d)\n", spilling, onFlash)
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// synthPopulateCmds builds the command(s) creating one synthetic key of the
// configured gSynthType. Compound types get gSynthItems items of size len(val).
func synthPopulateCmds(c valkey.Client, key, val string) valkey.Commands {
	n := gSynthItems
	if n < 1 {
		n = 1
	}
	switch gSynthType {
	case "hash":
		args := make([]string, 0, n*2)
		for i := 0; i < n; i++ {
			args = append(args, "f"+strconv.Itoa(i), val)
		}
		return valkey.Commands{c.B().Arbitrary("HSET", key).Args(args...).Build()}
	case "list":
		args := make([]string, n)
		for i := range args {
			args[i] = val
		}
		return valkey.Commands{c.B().Arbitrary("RPUSH", key).Args(args...).Build()}
	case "set":
		args := make([]string, n)
		for i := range args {
			args[i] = "e" + strconv.Itoa(i) + ":" + val
		}
		return valkey.Commands{c.B().Arbitrary("SADD", key).Args(args...).Build()}
	case "zset":
		args := make([]string, 0, n*2)
		for i := 0; i < n; i++ {
			args = append(args, strconv.Itoa(i), "e"+strconv.Itoa(i)+":"+val)
		}
		return valkey.Commands{c.B().Arbitrary("ZADD", key).Args(args...).Build()}
	case "stream":
		cmds := make(valkey.Commands, n)
		for i := range cmds {
			cmds[i] = c.B().Arbitrary("XADD", key).Args("*", "f", val).Build()
		}
		return cmds
	default: // string
		return valkey.Commands{c.B().Set().Key(key).Value(val).Build()}
	}
}

func doSyntheticPopulate(addr string, numKeys, valSize int, prefix string, keySize, batchSize int) {
	client, err := valkey.NewClient(valkey.ClientOption{InitAddress: []string{addr}, DisableCache: true})
	if err != nil {
		fmt.Fprintf(os.Stderr, "populate connect error: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	fmt.Printf("  Populating %d synthetic keys (%s*, %d bytes each, pipeline=%d)...\n", numKeys, prefix, valSize, batchSize)
	ctx := context.Background()
	val := strings.Repeat("x", valSize)
	batch := make(valkey.Commands, 0, batchSize)
	sent, errs := 0, 0
	start := time.Now()

	for i := 0; i < numKeys; i++ {
		key := synthKey(prefix, i, keySize)
		batch = append(batch, synthPopulateCmds(client, key, val)...)
		if len(batch) >= batchSize {
			resps := client.DoMulti(ctx, batch...)
			for _, r := range resps {
				if r.Error() != nil {
					errs++
				}
			}
			sent += len(batch)
			batch = batch[:0]
			if sent%50000 == 0 {
				elapsed := time.Since(start).Seconds()
				fmt.Printf("    %d/%d (%.0f keys/s, %d errors)\n", sent, numKeys, float64(sent)/elapsed, errs)
			}
		}
	}
	if len(batch) > 0 {
		resps := client.DoMulti(ctx, batch...)
		for _, r := range resps {
			if r.Error() != nil {
				errs++
			}
		}
		sent += len(batch)
	}

	elapsed := time.Since(start).Seconds()
	fmt.Printf("  Done. %d keys in %.1fs (%.0f keys/s, %d errors)\n", sent, elapsed, float64(sent)/elapsed, errs)
}

func replay(addr string, ops []Op, numClients, targetRPS int, speed float64, checkpointInterval int) (*Stats, time.Duration) {
	clients := make([]valkey.Client, numClients)
	for i := range clients {
		c, err := valkey.NewClient(valkey.ClientOption{
			InitAddress:       []string{addr},
			DisableCache:      true,
			PipelineMultiplex: -1,
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "connect error (client %d): %v\n", i, err)
			os.Exit(1)
		}
		defer c.Close()
		clients[i] = c
	}

	metricClient, _ := valkey.NewClient(valkey.ClientOption{InitAddress: []string{addr}, DisableCache: true})
	defer metricClient.Close()

	stats := &Stats{latencies: make([]time.Duration, 0, len(ops))}
	ctx := context.Background()

	// Two-phase replay: send batch → drain → measure → next batch
	var checkpoints []Checkpoint
	var lastReads, lastWrites, lastSectorsR, lastSectorsW int64
	lastReads, lastWrites, lastSectorsR, lastSectorsW = readBlockStats()
	var lastCPFetches int64

	start := time.Now()
	opsIdx := 0

	for opsIdx < len(ops) {
		// Determine batch size
		batchEnd := opsIdx + checkpointInterval
		if checkpointInterval <= 0 || batchEnd > len(ops) {
			batchEnd = len(ops)
		}
		batch := ops[opsIdx:batchEnd]
		opsIdx = batchEnd

		// Phase 1: send batch and drain
		work := make(chan Op, numClients*2)
		var wg sync.WaitGroup
		for i := 0; i < numClients; i++ {
			wg.Add(1)
			go func(c valkey.Client) {
				defer wg.Done()
				for op := range work {
					t0 := time.Now()
					execOp(ctx, c, op, stats)
					stats.recordLatency(time.Since(t0))
				}
			}(clients[i])
		}

		intervalStart := time.Now()
		for i, op := range batch {
			if targetRPS > 0 {
				expected := time.Duration(opsIdx-len(batch)+i) * time.Second / time.Duration(targetRPS)
				if wait := expected - time.Since(start); wait > 0 {
					time.Sleep(wait)
				}
			} else if speed > 0 {
				expected := time.Duration(float64(time.Duration(op.Timestamp)*time.Second) / speed)
				if wait := expected - time.Since(start); wait > 0 {
					time.Sleep(wait)
				}
			}
			work <- op
		}
		close(work)
		wg.Wait()
		intervalDuration := time.Since(intervalStart)

		// Phase 2: measure (all ops drained, no contention)
		if checkpointInterval > 0 {
			stats.mu.Lock()
			lats := make([]time.Duration, len(stats.latencies))
			copy(lats, stats.latencies)
			stats.latencies = stats.latencies[:0]
			stats.mu.Unlock()

			sort.Slice(lats, func(i, j int) bool { return lats[i] < lats[j] })
			n := len(lats)
			var p50, p99, p999 float64
			if n > 0 {
				p50 = float64(lats[n*50/100].Microseconds()) / 1000
				p99 = float64(lats[n*99/100].Microseconds()) / 1000
				p999 = float64(lats[min(n*999/1000, n-1)].Microseconds()) / 1000
			}

			secs := intervalDuration.Seconds()
			var usedMem float64
			var tieredVals, tieredFetch int64
			if info, err := metricClient.Do(ctx, metricClient.B().Info().Section("memory", "stats", "external_storage").Build()).ToString(); err == nil {
				for _, line := range strings.Split(info, "\n") {
					line = strings.TrimSpace(line)
					if strings.HasPrefix(line, "used_memory:") {
						v, _ := strconv.ParseFloat(strings.TrimPrefix(line, "used_memory:"), 64)
						usedMem = v / 1024 / 1024
					} else if strings.HasPrefix(line, "completion_write_ok:") {
						tieredVals, _ = strconv.ParseInt(strings.TrimPrefix(line, "completion_write_ok:"), 10, 64)
					} else if strings.HasPrefix(line, "total_num_items_fetched_from_ext_storage:") {
						tieredFetch, _ = strconv.ParseInt(strings.TrimPrefix(line, "total_num_items_fetched_from_ext_storage:"), 10, 64)
					}
				}
			}
			intervalFetches := tieredFetch - lastCPFetches
			lastCPFetches = tieredFetch

			curReads, curWrites, curSectorsR, curSectorsW := readBlockStats()
			cp := Checkpoint{
				OpsCompleted:    int64(opsIdx),
				ElapsedS:        secs,
				Throughput:      float64(len(batch)) / secs,
				P50Ms:           p50,
				P99Ms:           p99,
				P999Ms:          p999,
				HitRatio:        float64(stats.hits.Load()) / float64(max(stats.hits.Load()+stats.misses.Load(), 1)) * 100,
				UsedMemoryMB:    usedMem,
				TieredValues:    tieredVals,
				TieredFetches:   tieredFetch,
				IntervalFetches: intervalFetches,
				FetchRatioPct:   float64(intervalFetches) / float64(max(len(batch), 1)) * 100,
				WriteIOPS:       float64(curWrites-lastWrites) / secs,
				WriteMBs:        float64(curSectorsW-lastSectorsW) * 512 / 1024 / 1024 / secs,
				ReadIOPS:        float64(curReads-lastReads) / secs,
				ReadMBs:         float64(curSectorsR-lastSectorsR) * 512 / 1024 / 1024 / secs,
			}
			lastReads, lastWrites, lastSectorsR, lastSectorsW = curReads, curWrites, curSectorsR, curSectorsW
			checkpoints = append(checkpoints, cp)

			fmt.Printf("    [%dK ops] %.0f ops/s  p50=%.2fms  p99=%.2fms  p999=%.2fms  mem=%.0fMB  tiered=%d  fetches=%d (%.1f%%)\n",
				opsIdx/1000, cp.Throughput, cp.P50Ms, cp.P99Ms, cp.P999Ms, cp.UsedMemoryMB, cp.TieredValues, cp.IntervalFetches,
				cp.FetchRatioPct)
		}
	}

	stats.checkpoints = checkpoints
	return stats, time.Since(start)
}

func execOp(ctx context.Context, client valkey.Client, op Op, stats *Stats) {
	switch op.Type {
	case "SET": // unconditional write (memcached "set")
		val := strings.Repeat("x", max(op.Size, 1))
		cmd := client.B().Set().Key(op.Key).Value(val)
		if op.TTL > 0 {
			cmd.Ex(time.Duration(op.TTL) * time.Second)
		}
		if err := client.Do(ctx, cmd.Build()).Error(); err != nil {
			stats.errors.Add(1)
		} else {
			stats.hits.Add(1)
		}
	case "ADD": // write-if-absent (memcached "add")
		val := strings.Repeat("x", max(op.Size, 1))
		cmd := client.B().Set().Key(op.Key).Value(val).Nx()
		if op.TTL > 0 {
			cmd.Ex(time.Duration(op.TTL) * time.Second)
		}
		if err := client.Do(ctx, cmd.Build()).Error(); err != nil {
			stats.errors.Add(1)
		} else {
			stats.hits.Add(1)
		}
	case "CAS": // update-if-exists (memcached "cas")
		val := strings.Repeat("x", max(op.Size, 1))
		cmd := client.B().Set().Key(op.Key).Value(val).Xx()
		if op.TTL > 0 {
			cmd.Ex(time.Duration(op.TTL) * time.Second)
		}
		if err := client.Do(ctx, cmd.Build()).Error(); err != nil {
			stats.errors.Add(1)
		} else {
			stats.hits.Add(1)
		}
	case "DEL": // delete
		if err := client.Do(ctx, client.B().Del().Key(op.Key).Build()).Error(); err != nil {
			stats.errors.Add(1)
		} else {
			stats.hits.Add(1)
		}
	case "SYNTHREAD": // full-object read of a compound synth key (triggers tiering fetch)
		var cmd valkey.Completed
		switch gSynthType {
		case "hash":
			cmd = client.B().Arbitrary("HGETALL", op.Key).Build()
		case "list":
			cmd = client.B().Arbitrary("LRANGE", op.Key, "0", "-1").Build()
		case "set":
			cmd = client.B().Arbitrary("SMEMBERS", op.Key).Build()
		case "zset":
			cmd = client.B().Arbitrary("ZRANGE", op.Key, "0", "-1").Build()
		case "stream":
			cmd = client.B().Arbitrary("XRANGE", op.Key, "-", "+").Build()
		default:
			cmd = client.B().Get().Key(op.Key).Build()
		}
		if err := client.Do(ctx, cmd).Error(); err != nil {
			stats.errors.Add(1)
		} else {
			stats.hits.Add(1)
		}
	default: // GET, GETS
		resp := client.Do(ctx, client.B().Get().Key(op.Key).Build())
		if err := resp.Error(); err != nil {
			if valkey.IsValkeyNil(err) {
				if gSetOnMiss {
					val := strings.Repeat("x", max(op.Size, 1))
					if err2 := client.Do(ctx, client.B().Set().Key(op.Key).Value(val).Build()).Error(); err2 != nil {
						stats.errors.Add(1)
					} else {
						stats.misses.Add(1)
					}
				} else {
					stats.misses.Add(1)
				}
			} else {
				stats.errors.Add(1)
			}
		} else {
			stats.hits.Add(1)
		}
	}
}

func buildResult(stats *Stats, elapsed time.Duration, clients int, speed float64, scenario string) Result {
	hits := stats.hits.Load()
	misses := stats.misses.Load()
	errs := stats.errors.Load()
	total := hits + misses + errs

	stats.mu.Lock()
	lats := stats.latencies
	stats.mu.Unlock()
	sort.Slice(lats, func(i, j int) bool { return lats[i] < lats[j] })
	n := len(lats)

	var p50, p99, p999, p100 float64
	if n > 0 {
		p50 = float64(lats[n*50/100].Microseconds()) / 1000
		p99 = float64(lats[n*99/100].Microseconds()) / 1000
		p999 = float64(lats[min(n*999/1000, n-1)].Microseconds()) / 1000
		p100 = float64(lats[n-1].Microseconds()) / 1000
	} else if len(stats.checkpoints) > 0 {
		// Use last checkpoint's per-interval latencies as final
		last := stats.checkpoints[len(stats.checkpoints)-1]
		p50, p99, p999 = last.P50Ms, last.P99Ms, last.P999Ms
	}

	return Result{
		Scenario:    scenario,
		Clients:     clients,
		Speed:       speed,
		Ops:         total,
		Errors:      errs,
		DurationS:   elapsed.Seconds(),
		Throughput:  float64(total) / elapsed.Seconds(),
		HitRatio:    float64(hits) / float64(max(hits+misses, 1)) * 100,
		P50Ms:       p50,
		P99Ms:       p99,
		P999Ms:      p999,
		P100Ms:      p100,
		Checkpoints: stats.checkpoints,
	}
}

func printResult(r Result) {
	fmt.Printf("\n%s\n", strings.Repeat("=", 60))
	if r.Scenario != "" {
		fmt.Printf("  %s\n", r.Scenario)
	}
	fmt.Printf("  Trace Replay Results\n")
	fmt.Printf("%s\n", strings.Repeat("=", 60))
	fmt.Printf("  Clients:    %d\n", r.Clients)
	fmt.Printf("  Speed:      %.1fx\n", r.Speed)
	fmt.Printf("  Ops:        %d (%d errors)\n", r.Ops, r.Errors)
	fmt.Printf("  Duration:   %.2fs\n", r.DurationS)
	fmt.Printf("  Throughput: %.0f ops/s\n", r.Throughput)
	fmt.Printf("  Hit ratio:  %.1f%%\n", r.HitRatio)
	fmt.Printf("  Latency:    p50=%.2fms  p99=%.2fms  p99.9=%.2fms  p100=%.2fms\n", r.P50Ms, r.P99Ms, r.P999Ms, r.P100Ms)
	fmt.Printf("%s\n\n", strings.Repeat("=", 60))
}

func appendJSON(path string, r Result) {
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Fprintf(os.Stderr, "json write error: %v\n", err)
		return
	}
	defer f.Close()
	b, _ := json.Marshal(r)
	f.Write(b)
	f.WriteString("\n")
}

// streamPopulate reads the trace line-by-line, deduplicates keys, and sends pipelined SETs.
// Memory usage: O(unique_keys) for the seen map, not O(total_ops).
func streamPopulate(addr, path string, maxKeys, batchSize int) {
	client, err := valkey.NewClient(valkey.ClientOption{InitAddress: []string{addr}, DisableCache: true})
	if err != nil {
		fmt.Fprintf(os.Stderr, "populate connect error: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	f, err := os.Open(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open error: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	r := csv.NewReader(f)
	r.FieldsPerRecord = -1
	r.TrimLeadingSpace = true
	r.Read() // skip header

	seen := make(map[string]struct{})
	ctx := context.Background()
	batch := make(valkey.Commands, 0, batchSize)
	sent, errs := 0, 0
	start := time.Now()

	fmt.Printf("  Streaming populate from %s (batch=%d)...\n", path, batchSize)

	for {
		row, err := r.Read()
		if err == io.EOF {
			break
		}
		if err != nil || len(row) < 4 {
			continue
		}
		key := strings.TrimRight(row[1], "\r")
		if _, ok := seen[key]; ok {
			continue
		}
		seen[key] = struct{}{}
		if maxKeys > 0 && len(seen) > maxKeys {
			break
		}

		size, _ := strconv.Atoi(strings.TrimRight(row[3], "\r"))
		if size < 1 {
			size = 1
		}
		val := strings.Repeat("x", size)
		batch = append(batch, client.B().Set().Key(key).Value(val).Build())

		if len(batch) >= batchSize {
			resps := client.DoMulti(ctx, batch...)
			for _, resp := range resps {
				if resp.Error() != nil {
					errs++
				}
			}
			sent += len(batch)
			batch = batch[:0]
			if sent%100000 == 0 {
				elapsed := time.Since(start).Seconds()
				fmt.Printf("    %d keys (%.0f keys/s, %d errors)\n", sent, float64(sent)/elapsed, errs)
			}
		}
	}
	if len(batch) > 0 {
		resps := client.DoMulti(ctx, batch...)
		for _, resp := range resps {
			if resp.Error() != nil {
				errs++
			}
		}
		sent += len(batch)
	}

	elapsed := time.Since(start).Seconds()
	fmt.Printf("  Done. %d keys in %.1fs (%.0f keys/s, %d errors)\n", sent, elapsed, float64(sent)/elapsed, errs)
}

func streamReplay(addr, path string, maxOps, numClients int, speed float64, checkpointInterval int) (*Stats, time.Duration) {
	// Create one dedicated connection per worker — no multiplexing/pipelining
	clients := make([]valkey.Client, numClients)
	for i := range clients {
		c, err := valkey.NewClient(valkey.ClientOption{
			InitAddress:       []string{addr},
			DisableCache:      true,
			PipelineMultiplex: -1,
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "connect error (client %d): %v\n", i, err)
			os.Exit(1)
		}
		defer c.Close()
		clients[i] = c
	}

	f, err := os.Open(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open error: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	r := csv.NewReader(f)
	r.FieldsPerRecord = -1
	r.TrimLeadingSpace = true
	r.Read() // skip header

	stats := &Stats{latencies: make([]time.Duration, 0, maxOps)}
	work := make(chan Op, numClients*2)
	var wg sync.WaitGroup
	ctx := context.Background()

	// Start worker goroutines — each owns one connection
	for i := 0; i < numClients; i++ {
		wg.Add(1)
		go func(c valkey.Client) {
			defer wg.Done()
			for op := range work {
				t0 := time.Now()
				execOp(ctx, c, op, stats)
				stats.recordLatency(time.Since(t0))
			}
		}(clients[i])
	}

	start := time.Now()
	count := 0

	for count < maxOps {
		row, err := r.Read()
		if err == io.EOF {
			break
		}
		if err != nil || len(row) < 4 {
			continue
		}

		ts, _ := strconv.Atoi(strings.TrimRight(row[0], "\r"))
		var key, opType string
		var size, ttl int

		if gTwitterMode {
			// Twitter format: timestamp,key,key_size,value_size,seq_num,op,ttl
			key = strings.TrimRight(row[1], "\r")
			size, _ = strconv.Atoi(strings.TrimRight(row[3], "\r"))
			opType = "GET"
			if len(row) >= 6 {
				opType = mapTraceOp(strings.TrimRight(row[5], "\r"))
			}
			if len(row) >= 7 {
				ttl, _ = strconv.Atoi(strings.TrimRight(row[6], "\r"))
			}
		} else {
			// Default format: timestamp,op,key,size
			opType = strings.TrimRight(row[1], "\r")
			key = strings.TrimRight(row[2], "\r")
			size, _ = strconv.Atoi(strings.TrimRight(row[3], "\r"))
		}

		// Rate control (only if speed > 0)
		if speed > 0 {
			expected := time.Duration(float64(time.Duration(ts)*time.Second) / speed)
			if wait := expected - time.Since(start); wait > 0 {
				time.Sleep(wait)
			}
		}

		work <- Op{Timestamp: ts, Type: opType, Key: key, Size: size, TTL: ttl}
		count++

		if count%1000000 == 0 {
			elapsed := time.Since(start).Seconds()
			fmt.Printf("    %dM ops (%.0f ops/s)\n", count/1000000, float64(count)/elapsed)
		}
	}

	close(work)
	wg.Wait()
	return stats, time.Since(start)
}

// readBlockStats reads /sys/block/nvme1n1/stat for I/O counters
func readBlockStats() (reads, writes, sectorsRead, sectorsWritten int64) {
	data, err := os.ReadFile("/sys/block/nvme1n1/stat")
	if err != nil {
		return 0, 0, 0, 0
	}
	fields := strings.Fields(string(data))
	if len(fields) >= 7 {
		reads, _ = strconv.ParseInt(fields[0], 10, 64)
		sectorsRead, _ = strconv.ParseInt(fields[2], 10, 64)
		writes, _ = strconv.ParseInt(fields[4], 10, 64)
		sectorsWritten, _ = strconv.ParseInt(fields[6], 10, 64)
	}
	return
}


