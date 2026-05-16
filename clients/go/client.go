// Go-Web frontend for the
// parados HTTP media server

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

// structs

// library item
type item struct {
	ID   string `json:"id"`
	Path string `json:"path"`
}
// upstream response
type library_resp struct {
	Proto int    `json:"proto"`
	Items []item `json:"items"`
}
// /meta/{id} response
type meta_resp struct {
	Proto int    `json:"proto"`
	ID    string `json:"id"`
	Path  string `json:"path"`
	Name  string `json:"name"`
	Size  uint64 `json:"size"`
	Mtime int64  `json:"mtime"`
	Type  string `json:"type"`
	Kind  string `json:"kind"`
}
// app state
type handler struct {
	parados  string
	web_dir  string
	httpc    *http.Client
}

// functions
func (h *handler) serve_file(w http.ResponseWriter, r *http.Request, path string, ctype string) {
	p := h.web_dir + "/" + path

	b, err := os.ReadFile(p)
	if err != nil {
		http.Error(w, "Web Error: "+p+": "+err.Error(), 500)
		return
	}

	if ctype != "" {
		w.Header().Set("Content-Type", ctype)
	}

	w.WriteHeader(200)
	w.Write(b)
}

func (h *handler) load_web(path string) (string, error) {
	p := h.web_dir + "/" + path

	b, err := os.ReadFile(p)
	if err != nil {
		return "", err
	}

	return string(b), nil
}

func (h *handler) web_subst(w http.ResponseWriter, body string, kv map[string]string) {
	for k, v := range kv {
		body = strings.ReplaceAll(body, k, v)
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(200)
	w.Write([]byte(body))
}

func (h *handler) pass_auth(dst *http.Request, src *http.Request) {
	a := src.Header.Get("Authorization")
	if a != "" {
		dst.Header.Set("Authorization", a)
	}
}

func (h *handler) upstream_req(method string, path string, r *http.Request) *http.Request {
	req, _ := http.NewRequest(method, h.parados+path, nil)
	h.pass_auth(req, r)
	return req
}

func (h *handler) upstream_json(path string, r *http.Request, out any) (int, error) {
	req := h.upstream_req("GET", path, r)

	resp, err := h.httpc.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		return resp.StatusCode, nil
	}

	return 200, json.NewDecoder(resp.Body).Decode(out)
}

func (h *handler) index(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" && r.URL.Path != "/index.html" {
		http.NotFound(w, r)
		return
	}

	h.serve_file(w, r, "index.html", "text/html; charset=utf-8")
}

func (h *handler) style(w http.ResponseWriter, r *http.Request) {
	h.serve_file(w, r, "style.css", "text/css; charset=utf-8")
}

func (h *handler) ping(w http.ResponseWriter, r *http.Request) {
	req := h.upstream_req("GET", "/ping", r)

	resp, err := h.httpc.Do(req)
	if err != nil {
		http.Error(w, "Upstream Error", 502)
		return
	}
	defer resp.Body.Close()

	w.WriteHeader(resp.StatusCode)

	if r.Method == "HEAD" {
		return
	}

	io.Copy(w, resp.Body)
}

func (h *handler) library(w http.ResponseWriter, r *http.Request) {
	var lr library_resp
	code, err := h.upstream_json("/library", r, &lr)
	if err != nil {
		http.Error(w, "Upstream Error", 502)
		return
	}

	if code == 401 {
		w.Header().Set("WWW-Authenticate", `Basic realm="parados"`)
		http.Error(w, "Unauthorized", 401)
		return
	}

	if code != 200 {
		http.Error(w, http.StatusText(code), code)
		return
	}

	if lr.Proto != 1 {
		http.Error(w, "Unknown Proto Version", 502)
		return
	}

	body, err := h.load_web("library.html")
	if err != nil {
		http.Error(w, "Web Error: "+h.web_dir+"/library.html: "+err.Error(), 500)
		return
	}

	var items strings.Builder
	for _, it := range lr.Items {
		fmt.Fprintf(&items, `<li><a href="/item/%s">%s</a></li>`+"\n", it.ID, it.Path)
	}

	h.web_subst(w, body, map[string]string{
		"{{COUNT}}": fmt.Sprintf("%d", len(lr.Items)),
		"{{ITEMS}}": items.String(),
	})
}

func (h *handler) item(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/item/")
	if id == "" {
		http.NotFound(w, r)
		return
	}

	var mr meta_resp
	code, err := h.upstream_json("/meta/"+id, r, &mr)
	if err != nil {
		http.Error(w, "Upstream Error", 502)
		return
	}

	if code == 401 {
		w.Header().Set("WWW-Authenticate", `Basic realm="parados"`)
		http.Error(w, "Unauthorized", 401)
		return
	}

	if code != 200 {
		http.Error(w, http.StatusText(code), code)
		return
	}

	if mr.Proto != 1 {
		http.Error(w, "Unknown Proto Version", 502)
		return
	}

	body, err := h.load_web("item.html")
	if err != nil {
		http.Error(w, "Web Error: "+h.web_dir+"/item.html: "+err.Error(), 500)
		return
	}

	var meta strings.Builder
	fmt.Fprintf(&meta, "<li>id: <code>%s</code></li>\n", mr.ID)
	fmt.Fprintf(&meta, "<li>type: <code>%s</code></li>\n", mr.Type)
	fmt.Fprintf(&meta, "<li>kind: <code>%s</code></li>\n", mr.Kind)
	fmt.Fprintf(&meta, "<li>size: %d</li>\n", mr.Size)
	fmt.Fprintf(&meta, "<li>mtime: %d</li>\n", mr.Mtime)

	player := ""
	if mr.Kind == "video" {
		player = fmt.Sprintf(`<video controls preload="metadata" src="/stream/%s"></video>`, mr.ID)

	} else if mr.Kind == "audio" {
		player = fmt.Sprintf(`<audio controls preload="metadata" src="/stream/%s"></audio>`, mr.ID)

	} else if mr.Kind == "image" {
		player = fmt.Sprintf(`<p><img src="/stream/%s"></p>`, mr.ID)

	} else {
		player = fmt.Sprintf(`<p><a href="/stream/%s">download</a></p>`, mr.ID)
	}

	h.web_subst(w, body, map[string]string{
		"{{ID}}": mr.ID,
		"{{NAME}}": mr.Name,
		"{{PATH}}": mr.Path,
		"{{META}}": meta.String(),
		"{{PLAYER}}": player,
	})
}

func (h *handler) stream(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/stream/")
	if id == "" {
		http.NotFound(w, r)
		return
	}

	if r.Method != "GET" && r.Method != "HEAD" {
		http.Error(w, "Method Not Allowed", 405)
		return
	}

	req := h.upstream_req(r.Method, "/stream/"+id, r)

	rng := r.Header.Get("Range")
	if rng != "" {
		req.Header.Set("Range", rng)
	}

	resp, err := h.httpc.Do(req)
	if err != nil {
		http.Error(w, "Upstream Error", 502)
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode == 401 {
		w.Header().Set("WWW-Authenticate", `Basic realm="parados"`)
		http.Error(w, "Unauthorized", 401)
		return
	}

	ct := resp.Header.Get("Content-Type")
	if ct != "" {
		w.Header().Set("Content-Type", ct)
	}

	cl := resp.Header.Get("Content-Length")
	if cl != "" {
		w.Header().Set("Content-Length", cl)
	}

	cr := resp.Header.Get("Content-Range")
	if cr != "" {
		w.Header().Set("Content-Range", cr)
	}

	ar := resp.Header.Get("Accept-Ranges")
	if ar != "" {
		w.Header().Set("Accept-Ranges", ar)
	}

	cd := resp.Header.Get("Content-Disposition")
	if cd != "" {
		w.Header().Set("Content-Disposition", cd)
	}

	w.WriteHeader(resp.StatusCode)

	if r.Method == "HEAD" {
		return
	}

	io.Copy(w, resp.Body)
}

func main() {
	listen := flag.String("listen", "0.0.0.0:8808", "Listen Addr")
	par := flag.String("parados", "http://127.0.0.1:8088", "Parados URL")
	web := flag.String("web", "web", "Web-Content Dir")
	flag.Parse()

	h := &handler{
		parados: *par,
		web_dir: *web,
		httpc: &http.Client{Timeout: 15 * time.Second},
	}

	// routes
	mux := http.NewServeMux()
	mux.HandleFunc("/", h.index)
	mux.HandleFunc("/style.css", h.style)
	mux.HandleFunc("/ping", h.ping)
	mux.HandleFunc("/library", h.library)
	mux.HandleFunc("/item/", h.item)
	mux.HandleFunc("/stream/", h.stream)

	// start server
	fmt.Fprintf(os.Stderr, "listening on %s\n", *listen)
	if err := http.ListenAndServe(*listen, mux); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

