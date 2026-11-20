package studio

import (
	"io"
	"net/http"
	"strconv"
	"sync"

	"github.com/pkg/errors"
)

// RenderPage is the page's address, relative to the server's root, that
// draws the render job instead of browsing.
const RenderPage = "/?render"

// Render hands a list of shots to the page at RenderPage and passes each
// picture it sends back to save. One page draws the job; a second page that
// opens it draws it again, and the later answer for a shot is saved over the
// first.
type Render struct {
	job  *RenderJob
	save func(index int, result *ShotResult) error

	// mu guards answered, left and err; done closes when every shot has an
	// answer.
	mu       sync.Mutex
	answered []bool
	left     int
	err      error
	done     chan struct{}
}

// NewRender returns a render of shots that calls save once for each shot's
// first answer, from the server's goroutines one at a time.
func NewRender(shots []*Shot, save func(index int, result *ShotResult) error) *Render {
	r := &Render{
		job:      &RenderJob{Shots: shots},
		save:     save,
		answered: make([]bool, len(shots)),
		left:     len(shots),
		done:     make(chan struct{}),
	}
	if r.left == 0 {
		close(r.done)
	}
	return r
}

// Done closes when every shot has an answer.
func (r *Render) Done() <-chan struct{} {
	return r.done
}

// Err returns the first error save returned, or nil.
func (r *Render) Err() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.err
}

// routes adds the render routes to mux.
func (r *Render) routes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/render/job", func(w http.ResponseWriter, _ *http.Request) {
		writeMessage(w, r.job)
	})
	mux.HandleFunc("POST /api/render/shot/{index}", r.shot)
}

// shot saves one shot's answer.
func (r *Render) shot(w http.ResponseWriter, req *http.Request) {
	// Read which shot and its answer.
	index, err := strconv.Atoi(req.PathValue("index"))
	if err != nil || index < 0 || index >= len(r.answered) {
		http.Error(w, "no such shot", http.StatusNotFound)
		return
	}
	body, err := io.ReadAll(req.Body)
	result := &ShotResult{}
	if err == nil {
		err = result.UnmarshalVT(body)
	}
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	// Save it, and finish when it was the last.
	r.mu.Lock()
	defer r.mu.Unlock()
	if err := r.save(index, result); err != nil {
		if r.err == nil {
			r.err = errors.Wrapf(err, "shot %d", index)
		}
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
	if r.answered[index] {
		return
	}
	r.answered[index] = true
	if r.left--; r.left == 0 {
		close(r.done)
	}
}
