package session

import (
	"context"
	"errors"
	"strconv"
	"time"

	"github.com/redis/go-redis/v9"
)

// Session is the Valkey record `sess:<id>` (05 §1.3). Valkey is never the truth: a lost record
// is rebuilt from a valid reconnect ticket.
type Session struct {
	ID           uint64
	AccountID    int64
	CharacterID  int64
	ZoneID       int64
	ContentBuild uint64
	Epoch        uint64
	Flags        uint8 // connecttoken.Flag* policy bits carried across reconnects (FlagBot)
	CreatedAt    time.Time
	Ended        bool
}

// Store errors.
var (
	ErrNotFound      = errors.New("session: not found")
	ErrEpochMismatch = errors.New("session: epoch mismatch (ticket superseded)")
	ErrEnded         = errors.New("session: ended")
	ErrSuperseded    = errors.New("session: account has a newer session")
	ErrIDCollision   = errors.New("session: session id already in use")
)

// Store keeps sessions in Valkey. All mutations are single Lua scripts, so they are atomic
// without WATCH/MULTI round trips. Safe for concurrent use.
type Store struct {
	rdb redis.Cmdable
}

// NewStore wraps a Valkey client.
func NewStore(rdb redis.Cmdable) *Store { return &Store{rdb: rdb} }

func sessKey(id uint64) string     { return "sess:" + strconv.FormatUint(id, 10) }
func acctKey(account int64) string { return "sess:acct:" + strconv.FormatInt(account, 10) }

// create: KEYS sess, acct; ARGV id, account, character, zone, build, created, ttl_ms, flags,
// tombstone_ms, prev_key_prefix. Returns the account's previous session ID ("" if none), which
// is ended in the same script so it cannot be reconnected in between; "!" when the ID exists.
var createScript = redis.NewScript(`
if redis.call('EXISTS', KEYS[1]) == 1 then return '!' end
local prev = redis.call('GET', KEYS[2])
if prev and prev ~= ARGV[1] then
  local pk = ARGV[10] .. prev
  if redis.call('EXISTS', pk) == 1 then
    redis.call('HSET', pk, 'ended', '1')
    redis.call('PEXPIRE', pk, ARGV[9])
  end
end
redis.call('HSET', KEYS[1], 'acct', ARGV[2], 'char', ARGV[3], 'zone', ARGV[4], 'build', ARGV[5],
  'epoch', '1', 'created', ARGV[6], 'ended', '0', 'flags', ARGV[8])
redis.call('PEXPIRE', KEYS[1], ARGV[7])
redis.call('SET', KEYS[2], ARGV[1], 'PX', ARGV[7])
if prev and prev ~= ARGV[1] then return prev end
return ''
`)

// Create stores a new session at epoch 1 and makes it the account's current session. The
// account's previous session is ended in the same atomic step (tombstoned for tombstone, so its
// outstanding tickets cannot resurrect it) and its ID returned (0 if none) so the caller can kick
// it. ErrIDCollision means the (random) ID is taken; the caller draws another.
func (s *Store) Create(ctx context.Context, sess *Session, ttl, tombstone time.Duration) (uint64, error) {
	prev, err := createScript.Run(ctx, s.rdb, []string{sessKey(sess.ID), acctKey(sess.AccountID)},
		sess.ID, sess.AccountID, sess.CharacterID, sess.ZoneID, sess.ContentBuild, sess.CreatedAt.Unix(), ttl.Milliseconds(),
		sess.Flags, tombstone.Milliseconds(), "sess:").Text()
	if err != nil {
		return 0, err
	}
	switch prev {
	case "!":
		return 0, ErrIDCollision
	case "":
		sess.Epoch = 1
		return 0, nil
	}
	sess.Epoch = 1
	return strconv.ParseUint(prev, 10, 64)
}

// Get loads a session.
func (s *Store) Get(ctx context.Context, id uint64) (*Session, error) {
	m, err := s.rdb.HGetAll(ctx, sessKey(id)).Result()
	if err != nil {
		return nil, err
	}
	if len(m) == 0 {
		return nil, ErrNotFound
	}
	sess := &Session{ID: id, Ended: m["ended"] == "1"}
	sess.AccountID, _ = strconv.ParseInt(m["acct"], 10, 64)
	sess.CharacterID, _ = strconv.ParseInt(m["char"], 10, 64)
	sess.ZoneID, _ = strconv.ParseInt(m["zone"], 10, 64)
	sess.ContentBuild, _ = strconv.ParseUint(m["build"], 10, 64)
	sess.Epoch, _ = strconv.ParseUint(m["epoch"], 10, 64)
	flags, _ := strconv.ParseUint(m["flags"], 10, 8)
	sess.Flags = uint8(flags)
	created, _ := strconv.ParseInt(m["created"], 10, 64)
	sess.CreatedAt = time.Unix(created, 0).UTC()
	return sess, nil
}

// CurrentFor returns the account's current session ID (0 if none).
func (s *Store) CurrentFor(ctx context.Context, account int64) (uint64, error) {
	v, err := s.rdb.Get(ctx, acctKey(account)).Result()
	if errors.Is(err, redis.Nil) {
		return 0, nil
	}
	if err != nil {
		return 0, err
	}
	return strconv.ParseUint(v, 10, 64)
}

// touch: KEYS sess, acct; ARGV id, ttl_ms. Slides both TTLs of a live session.
var touchScript = redis.NewScript(`
if redis.call('HGET', KEYS[1], 'ended') ~= '0' then return 0 end
redis.call('PEXPIRE', KEYS[1], ARGV[2])
if redis.call('GET', KEYS[2]) == ARGV[1] then redis.call('PEXPIRE', KEYS[2], ARGV[2]) end
return 1
`)

// Touch slides the 24 h TTL (gateways do this implicitly every 60 s via ticket sealing).
func (s *Store) Touch(ctx context.Context, sess *Session, ttl time.Duration) error {
	return touchScript.Run(ctx, s.rdb, []string{sessKey(sess.ID), acctKey(sess.AccountID)}, sess.ID, ttl.Milliseconds()).Err()
}

// advance: KEYS sess, acct; ARGV expect, ttl_ms, id, account, character, zone, build, created,
// flags. Returns the new epoch, or -1 mismatch, -2 ended, -3 superseded.
var advanceScript = redis.NewScript(`
local cur = redis.call('HGET', KEYS[1], 'epoch')
local owner = redis.call('GET', KEYS[2])
if not cur then
  if owner and owner ~= ARGV[3] then return -3 end
  local e = tonumber(ARGV[1]) + 1
  redis.call('HSET', KEYS[1], 'acct', ARGV[4], 'char', ARGV[5], 'zone', ARGV[6], 'build', ARGV[7],
    'epoch', tostring(e), 'created', ARGV[8], 'ended', '0', 'flags', ARGV[9])
  redis.call('PEXPIRE', KEYS[1], ARGV[2])
  redis.call('SET', KEYS[2], ARGV[3], 'PX', ARGV[2])
  return e
end
if redis.call('HGET', KEYS[1], 'ended') == '1' then return -2 end
if owner and owner ~= ARGV[3] then return -3 end
if cur ~= ARGV[1] then return -1 end
local e = redis.call('HINCRBY', KEYS[1], 'epoch', 1)
redis.call('PEXPIRE', KEYS[1], ARGV[2])
if owner then redis.call('PEXPIRE', KEYS[2], ARGV[2]) else redis.call('SET', KEYS[2], ARGV[3], 'PX', ARGV[2]) end
return e
`)

// AdvanceEpoch is the reconnect CAS (05 §1.3): if the session is at epoch expect it moves to
// expect+1, which tells the old gateway to drop its copy. A session missing from Valkey (cache
// loss) is rebuilt from the ticket in restore, unless the account has since started another.
func (s *Store) AdvanceEpoch(ctx context.Context, expect uint64, restore *Session, ttl time.Duration) (uint64, error) {
	r, err := advanceScript.Run(ctx, s.rdb, []string{sessKey(restore.ID), acctKey(restore.AccountID)},
		expect, ttl.Milliseconds(), restore.ID, restore.AccountID, restore.CharacterID, restore.ZoneID, restore.ContentBuild,
		restore.CreatedAt.Unix(), restore.Flags).Int64()
	if err != nil {
		return 0, err
	}
	switch r {
	case -1:
		return 0, ErrEpochMismatch
	case -2:
		return 0, ErrEnded
	case -3:
		return 0, ErrSuperseded
	}
	return uint64(r), nil
}

// restore: KEYS sess, acct; ARGV id, ttl_ms, account, character, zone, build, epoch, created, flags.
// Recreates a record lost with Valkey unless one exists (returns 0) or the account has moved
// on to another session (-3); returns 1 when it wrote the record.
var restoreScript = redis.NewScript(`
if redis.call('EXISTS', KEYS[1]) == 1 then return 0 end
local owner = redis.call('GET', KEYS[2])
if owner and owner ~= ARGV[1] then return -3 end
redis.call('HSET', KEYS[1], 'acct', ARGV[3], 'char', ARGV[4], 'zone', ARGV[5], 'build', ARGV[6],
  'epoch', ARGV[7], 'created', ARGV[8], 'ended', '0', 'flags', ARGV[9])
redis.call('PEXPIRE', KEYS[1], ARGV[2])
redis.call('SET', KEYS[2], ARGV[1], 'PX', ARGV[2])
return 1
`)

// Restore rebuilds a session record from a reconnect ticket's content (sess.Epoch = the
// ticket's epoch) when Valkey lost it. An existing record is left alone; ErrSuperseded means the
// account has another current session.
func (s *Store) Restore(ctx context.Context, sess *Session, ttl time.Duration) error {
	r, err := restoreScript.Run(ctx, s.rdb, []string{sessKey(sess.ID), acctKey(sess.AccountID)}, sess.ID, ttl.Milliseconds(),
		sess.AccountID, sess.CharacterID, sess.ZoneID, sess.ContentBuild, sess.Epoch, sess.CreatedAt.Unix(), sess.Flags).Int64()
	if err != nil {
		return err
	}
	if r == -3 {
		return ErrSuperseded
	}
	return nil
}

// end: KEYS sess, acct; ARGV id, tombstone_ms. Marks ended and keeps a tombstone long enough
// that outstanding tickets cannot resurrect the session.
var endScript = redis.NewScript(`
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
redis.call('HSET', KEYS[1], 'ended', '1')
redis.call('PEXPIRE', KEYS[1], ARGV[2])
if redis.call('GET', KEYS[2]) == ARGV[1] then redis.call('DEL', KEYS[2]) end
return 1
`)

// End terminates a session (logout, supersede, ban). ended reports whether it existed.
func (s *Store) End(ctx context.Context, id uint64, account int64, tombstone time.Duration) (bool, error) {
	n, err := endScript.Run(ctx, s.rdb, []string{sessKey(id), acctKey(account)}, id, tombstone.Milliseconds()).Int()
	return n == 1, err
}

// Ping checks Valkey.
func (s *Store) Ping(ctx context.Context) error { return s.rdb.Ping(ctx).Err() }
