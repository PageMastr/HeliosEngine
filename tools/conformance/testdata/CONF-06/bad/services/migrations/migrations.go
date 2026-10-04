package migrations

type Schema struct {
	Name, Dir, Legacy string
	LegacyVersion     int64
}

// chat's files up to 1 were written against the legacy schema "chat"; Up renames it after them.
var Schemas = []Schema{
	{Name: "svc_chat", Dir: "chat", Legacy: "chat", LegacyVersion: 1},
	{Name: "market", Dir: "market"},
}
