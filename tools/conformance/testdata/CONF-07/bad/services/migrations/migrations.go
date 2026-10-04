package migrations

type Schema struct {
	Name, Dir string
}

var Schemas = []Schema{{Name: "svc_identity", Dir: "identity"}, {Name: "svc_chat", Dir: "chat"}}
