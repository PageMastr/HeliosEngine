package migrations

type Schema struct {
	Name, Dir, Legacy string
	LegacyVersion     int64
}

const chatSchema = "svc_chat"

// Up applies the schemas in this order, zeta first.
var Schemas = []Schema{{Name: "svc_zeta", Dir: "zeta"}, {Name: chatSchema, Dir: "chat", Legacy: "chat", LegacyVersion: 1}}
