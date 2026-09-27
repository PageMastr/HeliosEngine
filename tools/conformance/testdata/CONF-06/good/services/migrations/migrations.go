package migrations

type Schema struct {
	Name, Dir, Legacy string
	LegacyVersion     int64
}

const chatSchema = "svc_chat"

var Schemas = []Schema{{Name: chatSchema, Dir: "chat", Legacy: "chat", LegacyVersion: 1}}
