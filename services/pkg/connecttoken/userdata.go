package connecttoken

import (
	"encoding/binary"
	"errors"
)

// UserDataVersion1 is the first Helios layout of the 256-byte netcode user data.
const UserDataVersion1 = 1

// User data flags.
const (
	FlagReconnect uint8 = 1 << 0 // token was minted by Reconnect(ticket), not by a fresh login
	FlagBot       uint8 = 1 << 1 // bot-scoped credential; only test shards accept it (05 §1.1)
)

// UserData is the Helios payload carried in the private part of every connect token (04 §2.3).
// Gateways read it after decrypting the token; clients never see it. Layout v1, little-endian,
// 8-byte aligned so the C++ side can read it with plain loads:
//
//	off  size  field
//	  0     1  layout version (1)
//	  1     1  flags (FlagReconnect, FlagBot)
//	  2     6  reserved, zero
//	  8     8  account_id
//	 16     8  character_id
//	 24     8  session_epoch
//	 32     8  content_build (content-version pin)
//	 40     8  zone_id (placement target)
//	 48     8  placement_ticket
//	 56     8  entitlements bitmask
//	 64    32  attestation hash
//	 96   160  reserved, zero
type UserData struct {
	Flags           uint8
	AccountID       uint64
	CharacterID     uint64
	SessionEpoch    uint64
	ContentBuild    uint64
	ZoneID          uint64
	PlacementTicket uint64
	Entitlements    uint64
	AttestationHash [32]byte
}

// ErrUserDataVersion is returned when the layout version byte is unknown.
var ErrUserDataVersion = errors.New("connecttoken: unknown user data layout version")

// Marshal encodes the v1 layout.
func (u *UserData) Marshal() [UserDataBytes]byte {
	var b [UserDataBytes]byte
	b[0] = UserDataVersion1
	b[1] = u.Flags
	le := binary.LittleEndian
	le.PutUint64(b[8:], u.AccountID)
	le.PutUint64(b[16:], u.CharacterID)
	le.PutUint64(b[24:], u.SessionEpoch)
	le.PutUint64(b[32:], u.ContentBuild)
	le.PutUint64(b[40:], u.ZoneID)
	le.PutUint64(b[48:], u.PlacementTicket)
	le.PutUint64(b[56:], u.Entitlements)
	copy(b[64:96], u.AttestationHash[:])
	return b
}

// UnmarshalUserData decodes the v1 layout. Reserved bytes are ignored so later minor
// revisions can add fields without breaking older gateways.
func UnmarshalUserData(b *[UserDataBytes]byte) (UserData, error) {
	if b[0] != UserDataVersion1 {
		return UserData{}, ErrUserDataVersion
	}
	le := binary.LittleEndian
	u := UserData{
		Flags:           b[1],
		AccountID:       le.Uint64(b[8:]),
		CharacterID:     le.Uint64(b[16:]),
		SessionEpoch:    le.Uint64(b[24:]),
		ContentBuild:    le.Uint64(b[32:]),
		ZoneID:          le.Uint64(b[40:]),
		PlacementTicket: le.Uint64(b[48:]),
		Entitlements:    le.Uint64(b[56:]),
	}
	copy(u.AttestationHash[:], b[64:96])
	return u, nil
}
